                    return false;
                }
            }
            return rotate_left(adjusted, output, error);
        }
        *output = root;
        return true;
    }

    static bool insert_front(
        const NodePtr& root,
        Chunk chunk,
        std::uint32_t max_records_per_chunk,
        NodePtr* output,
        std::string* error) {
        if (!root) {
            chunk.reserve(max_records_per_chunk);
            return make_node(std::move(chunk), nullptr, nullptr, output, error);
        }
        NodePtr next_left;
        if (!insert_front(root->left, std::move(chunk), max_records_per_chunk, &next_left, error)) {
            return false;
        }
        NodePtr rebuilt;
        if (!make_node(root->chunk, std::move(next_left), root->right, &rebuilt, error)) {
            return false;
        }
        return rebalance(rebuilt, output, error);
    }

    static bool extract_min(
        const NodePtr& root,
        ChunkPtr* minimum,
        NodePtr* remainder,
        std::string* error) {
        if (!root->left) {
            *minimum = root->chunk;
            *remainder = root->right;
            return true;
        }
        NodePtr next_left;
        if (!extract_min(root->left, minimum, &next_left, error)) {
            return false;
        }
        NodePtr rebuilt;
        if (!make_node(root->chunk, std::move(next_left), root->right, &rebuilt, error)) {
            return false;
        }
        return rebalance(rebuilt, remainder, error);
    }

    static bool join(const NodePtr& left, const NodePtr& right, NodePtr* output, std::string* error) {
        if (!left) {
            *output = right;
            return true;
        }
        if (!right) {
            *output = left;
            return true;
        }
        ChunkPtr minimum;
        NodePtr remainder;
        if (!extract_min(right, &minimum, &remainder, error)) {
            return false;
        }
        NodePtr rebuilt;
        if (!make_node(std::move(minimum), left, std::move(remainder), &rebuilt, error)) {
            return false;
        }
        return rebalance(rebuilt, output, error);
    }

    static bool insert_impl(
        const NodePtr& root,
        std::uint64_t index,
        const SequenceRecord& record,
        std::uint32_t max_records_per_chunk,
        NodePtr* output,
        std::string* error) {
        if (!root) {
            Chunk chunk;
            chunk.reserve(max_records_per_chunk);
            chunk.insert(0U, record);
            return make_node(std::move(chunk), nullptr, nullptr, output, error);
        }

        const std::uint64_t left_count = aggregate_of(root->left).record_count;
        const std::uint64_t chunk_count = static_cast<std::uint64_t>(root->chunk->size());
        NodePtr rebuilt;
        if (index < left_count) {
            NodePtr next_left;
            if (!insert_impl(root->left, index, record, max_records_per_chunk, &next_left, error) ||
                !make_node(root->chunk, std::move(next_left), root->right, &rebuilt, error)) {
                return false;
            }
        } else if (index <= left_count + chunk_count) {
            const std::uint64_t local64 = index - left_count;
            if (local64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                *error = "sequence local index overflow";
                return false;
            }
            Chunk chunk = *root->chunk;
            chunk.insert(static_cast<std::size_t>(local64), record);
            if (chunk.size() <= max_records_per_chunk) {
                if (!make_node(std::move(chunk), root->left, root->right, &rebuilt, error)) {
                    return false;
                }
            } else {
                const std::size_t split_at = chunk.size() / 2U;
                Chunk left_chunk;
                Chunk right_chunk;
                left_chunk.reserve(max_records_per_chunk);
                right_chunk.reserve(max_records_per_chunk);
                for (std::size_t item = 0U; item < split_at; ++item) {
                    const SequenceRecord value = chunk.at(item);
                    left_chunk.logical_ids.push_back(value.logical_id);
                    left_chunk.text_bytes.push_back(value.text_bytes);
                    left_chunk.heights_q8.push_back(value.height_q8);
                    left_chunk.search_summaries.push_back(value.search_summary);
                    left_chunk.source_record_indices.push_back(value.source_record_index);
                }
                for (std::size_t item = split_at; item < chunk.size(); ++item) {
                    const SequenceRecord value = chunk.at(item);
                    right_chunk.logical_ids.push_back(value.logical_id);
                    right_chunk.text_bytes.push_back(value.text_bytes);
                    right_chunk.heights_q8.push_back(value.height_q8);
                    right_chunk.search_summaries.push_back(value.search_summary);
                    right_chunk.source_record_indices.push_back(value.source_record_index);
                }
                NodePtr next_right;
                if (!insert_front(root->right, std::move(right_chunk), max_records_per_chunk, &next_right, error) ||
                    !make_node(std::move(left_chunk), root->left, std::move(next_right), &rebuilt, error)) {
                    return false;
                }
            }
        } else {
            NodePtr next_right;
            if (!insert_impl(
                    root->right,
                    index - left_count - chunk_count,
                    record,
                    max_records_per_chunk,
                    &next_right,
                    error) ||
                !make_node(root->chunk, root->left, std::move(next_right), &rebuilt, error)) {
                return false;
            }
        }
        return rebalance(rebuilt, output, error);
    }

    static bool erase_impl(
        const NodePtr& root,
        std::uint64_t index,
        SequenceRecord* erased,
        NodePtr* output,
        std::string* error) {
        if (!root) {
            *error = "sequence erase index is outside the sequence";
            return false;
        }
        const std::uint64_t left_count = aggregate_of(root->left).record_count;
        const std::uint64_t chunk_count = static_cast<std::uint64_t>(root->chunk->size());
        NodePtr rebuilt;
        if (index < left_count) {
            NodePtr next_left;
            if (!erase_impl(root->left, index, erased, &next_left, error) ||
                !make_node(root->chunk, std::move(next_left), root->right, &rebuilt, error)) {
                return false;
            }
        } else if (index >= left_count + chunk_count) {
            NodePtr next_right;
            if (!erase_impl(root->right, index - left_count - chunk_count, erased, &next_right, error) ||
                !make_node(root->chunk, root->left, std::move(next_right), &rebuilt, error)) {
                return false;
            }
        } else {
            Chunk chunk = *root->chunk;
            *erased = chunk.erase(static_cast<std::size_t>(index - left_count));
            if (chunk.size() == 0U) {
                return join(root->left, root->right, output, error);
            }
            if (!make_node(std::move(chunk), root->left, root->right, &rebuilt, error)) {
                return false;
            }
        }
        return rebalance(rebuilt, output, error);
    }

    static bool update_height_impl(
        const NodePtr& root,
        std::uint64_t index,
        std::uint32_t new_height_q8,
        std::uint32_t* old_height_q8,
        NodePtr* output,
        std::string* error) {
        const std::uint64_t left_count = aggregate_of(root->left).record_count;
        const std::uint64_t chunk_count = static_cast<std::uint64_t>(root->chunk->size());
        if (index < left_count) {
            NodePtr next_left;
            if (!update_height_impl(root->left, index, new_height_q8, old_height_q8, &next_left, error)) {
                return false;
            }
            return make_node(root->chunk, std::move(next_left), root->right, output, error);
        }
        if (index >= left_count + chunk_count) {
            NodePtr next_right;
            if (!update_height_impl(
                    root->right,
                    index - left_count - chunk_count,
                    new_height_q8,
                    old_height_q8,
                    &next_right,
                    error)) {
                return false;
            }
            return make_node(root->chunk, root->left, std::move(next_right), output, error);
        }
        Chunk chunk = *root->chunk;
        const std::size_t local = static_cast<std::size_t>(index - left_count);
        *old_height_q8 = chunk.heights_q8[local];
        chunk.heights_q8[local] = new_height_q8;