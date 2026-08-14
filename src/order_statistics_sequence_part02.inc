        return make_node(std::move(chunk), root->left, root->right, output, error);
    }

    static bool update_search_impl(
        const NodePtr& root,
        std::uint64_t index,
        std::uint64_t new_summary,
        std::uint64_t* old_summary,
        NodePtr* output,
        std::string* error) {
        const std::uint64_t left_count = aggregate_of(root->left).record_count;
        const std::uint64_t chunk_count = static_cast<std::uint64_t>(root->chunk->size());
        if (index < left_count) {
            NodePtr next_left;
            if (!update_search_impl(root->left, index, new_summary, old_summary, &next_left, error)) {
                return false;
            }
            return make_node(root->chunk, std::move(next_left), root->right, output, error);
        }
        if (index >= left_count + chunk_count) {
            NodePtr next_right;
            if (!update_search_impl(
                    root->right,
                    index - left_count - chunk_count,
                    new_summary,
                    old_summary,
                    &next_right,
                    error)) {
                return false;
            }
            return make_node(root->chunk, root->left, std::move(next_right), output, error);
        }
        Chunk chunk = *root->chunk;
        const std::size_t local = static_cast<std::size_t>(index - left_count);
        *old_summary = chunk.search_summaries[local];
        chunk.search_summaries[local] = new_summary;
        return make_node(std::move(chunk), root->left, root->right, output, error);
    }

    static bool prefix_impl(
        const Node* node,
        std::uint64_t count,
        SequenceAggregate* output,
        std::string* error) noexcept {
        if (node == nullptr) {
            if (count != 0U) {
                *error = "sequence prefix is outside the sequence";
                return false;
            }
            return true;
        }
        const std::uint64_t left_count = node->left ? node->left->aggregate.record_count : 0U;
        if (count <= left_count) {
            return prefix_impl(node->left.get(), count, output, error);
        }
        if (node->left) {
            if (!combine(*output, node->left->aggregate, SequenceAggregate{}, output)) {
                *error = "sequence prefix aggregate overflow";
                return false;
            }
        }
        std::uint64_t remaining = count - left_count;
        const std::uint64_t local_count = std::min<std::uint64_t>(remaining, node->chunk->size());
        for (std::uint64_t local = 0U; local < local_count; ++local) {
            const std::size_t item = static_cast<std::size_t>(local);
            if (!checked_add(output->record_count, 1U, &output->record_count) ||
                !checked_add(output->text_bytes, node->chunk->text_bytes[item], &output->text_bytes) ||
                !checked_add(output->layout_height_q8, node->chunk->heights_q8[item], &output->layout_height_q8)) {
                *error = "sequence prefix aggregate overflow";
                return false;
            }
            output->search_summary |= node->chunk->search_summaries[item];
        }
        remaining -= local_count;
        if (remaining == 0U) {
            return true;
        }
        return prefix_impl(node->right.get(), remaining, output, error);
    }

    static bool locate_by_records(
        const NodePtr& root,
        std::uint64_t index,
        SequencePosition* position,
        std::string* error) noexcept {
        const Node* node = root.get();
        std::uint64_t record_prefix = 0U;
        std::uint64_t text_prefix = 0U;
        std::uint64_t height_prefix = 0U;
        while (node != nullptr) {
            const SequenceAggregate left = node->left ? node->left->aggregate : SequenceAggregate{};
            if (index < record_prefix + left.record_count) {
                node = node->left.get();
                continue;
            }
            if (!checked_add(text_prefix, left.text_bytes, &text_prefix) ||
                !checked_add(height_prefix, left.layout_height_q8, &height_prefix) ||
                !checked_add(record_prefix, left.record_count, &record_prefix)) {
                *error = "sequence prefix overflow";
                return false;
            }
            const std::uint64_t local_limit = record_prefix + static_cast<std::uint64_t>(node->chunk->size());
            if (index < local_limit) {
                const std::size_t local = static_cast<std::size_t>(index - record_prefix);
                for (std::size_t item = 0U; item < local; ++item) {
                    if (!checked_add(text_prefix, node->chunk->text_bytes[item], &text_prefix) ||
                        !checked_add(height_prefix, node->chunk->heights_q8[item], &height_prefix)) {
                        *error = "sequence prefix overflow";
                        return false;
                    }
                }
                *position = SequencePosition{index, text_prefix, height_prefix, node->chunk->at(local)};
                return true;
            }
            for (std::size_t item = 0U; item < node->chunk->size(); ++item) {
                if (!checked_add(text_prefix, node->chunk->text_bytes[item], &text_prefix) ||
                    !checked_add(height_prefix, node->chunk->heights_q8[item], &height_prefix)) {
                    *error = "sequence prefix overflow";
                    return false;
                }
            }
            record_prefix = local_limit;
            node = node->right.get();
        }
        *error = "sequence record index is outside the sequence";
        return false;
    }

    enum class Metric { Text, Height };

    static bool locate_by_metric(
        const NodePtr& root,
        std::uint64_t offset,
        Metric metric,
        SequencePosition* position,
        std::string* error) noexcept {
        const Node* node = root.get();
        std::uint64_t record_prefix = 0U;
        std::uint64_t text_prefix = 0U;
        std::uint64_t height_prefix = 0U;
        while (node != nullptr) {
            const SequenceAggregate left = node->left ? node->left->aggregate : SequenceAggregate{};
            const std::uint64_t left_metric = metric == Metric::Text ? left.text_bytes : left.layout_height_q8;
            const std::uint64_t prefix_metric = metric == Metric::Text ? text_prefix : height_prefix;
            if (offset < prefix_metric + left_metric) {
                node = node->left.get();
                continue;
            }
            if (!checked_add(record_prefix, left.record_count, &record_prefix) ||
                !checked_add(text_prefix, left.text_bytes, &text_prefix) ||
                !checked_add(height_prefix, left.layout_height_q8, &height_prefix)) {
                *error = "sequence prefix overflow";
                return false;
            }
            for (std::size_t local = 0U; local < node->chunk->size(); ++local) {
                const std::uint64_t width = metric == Metric::Text ? node->chunk->text_bytes[local] : node->chunk->heights_q8[local];
                const std::uint64_t current_metric = metric == Metric::Text ? text_prefix : height_prefix;
                if (width != 0U && offset >= current_metric && offset - current_metric < width) {
                    *position = SequencePosition{record_prefix, text_prefix, height_prefix, node->chunk->at(local)};
                    return true;
                }
                if (!checked_add(record_prefix, 1U, &record_prefix) ||
                    !checked_add(text_prefix, node->chunk->text_bytes[local], &text_prefix) ||
                    !checked_add(height_prefix, node->chunk->heights_q8[local], &height_prefix)) {
                    *error = "sequence prefix overflow";
                    return false;
                }
            }
            node = node->right.get();
        }
        *error = metric == Metric::Text ? "sequence text offset is outside the sequence" : "sequence height offset is outside the sequence";
        return false;
    }

    static SequenceStats make_stats(const NodePtr& root, std::uint32_t max_records_per_chunk) noexcept {
        SequenceStats stats;
        stats.max_records_per_chunk = max_records_per_chunk;
        if (root) {
            stats.aggregate = root->aggregate;
            stats.chunk_count = root->chunk_count;
            stats.tree_height = root->tree_height;
        }
        return stats;
    }
};

ChunkedOrderStatisticsSequence::Snapshot::Snapshot(
    std::shared_ptr<const Impl> impl,
    SequenceStats stats) noexcept
    : impl_(std::move(impl)), stats_(stats) {}

const SequenceStats& ChunkedOrderStatisticsSequence::Snapshot::stats() const noexcept { return stats_; }
