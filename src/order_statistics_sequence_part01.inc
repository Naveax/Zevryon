                                     static_cast<std::int64_t>(height_of(root->right));
        if (balance > 1) {
            if (height_of(root->left->left) < height_of(root->left->right)) {
                root->left = rotate_left(std::move(root->left));
            }
            return rotate_right(std::move(root));
        }
        if (balance < -1) {
            if (height_of(root->right->right) < height_of(root->right->left)) {
                root->right = rotate_right(std::move(root->right));
            }
            return rotate_left(std::move(root));
        }
        return root;
    }

    static void insert_front(std::unique_ptr<Node>* slot, std::unique_ptr<Node> node) noexcept {
        if (!*slot) {
            *slot = std::move(node);
            return;
        }
        insert_front(&(*slot)->left, std::move(node));
        *slot = rebalance(std::move(*slot));
    }

    static std::unique_ptr<Node> extract_min(std::unique_ptr<Node>* slot) noexcept {
        if (!(*slot)->left) {
            auto minimum = std::move(*slot);
            *slot = std::move(minimum->right);
            minimum->right.reset();
            static_cast<void>(refresh(minimum.get()));
            return minimum;
        }
        auto minimum = extract_min(&(*slot)->left);
        *slot = rebalance(std::move(*slot));
        return minimum;
    }

    static std::unique_ptr<Node> join(std::unique_ptr<Node> left, std::unique_ptr<Node> right) noexcept {
        if (!left) {
            return right;
        }
        if (!right) {
            return left;
        }
        auto successor = extract_min(&right);
        successor->left = std::move(left);
        successor->right = std::move(right);
        return rebalance(std::move(successor));
    }

    bool split_chunk_node(std::unique_ptr<Node>* slot, std::string* error) {
        Node* current = slot->get();
        if (current == nullptr || current->chunk.size() <= max_records_per_chunk) {
            return true;
        }
        const std::size_t split_at = current->chunk.size() / 2U;
        Chunk right_chunk;
        right_chunk.reserve(max_records_per_chunk);
        for (std::size_t index = split_at; index < current->chunk.size(); ++index) {
            const SequenceRecord record = current->chunk.at(index);
            right_chunk.logical_ids.push_back(record.logical_id);
            right_chunk.text_bytes.push_back(record.text_bytes);
            right_chunk.heights_q8.push_back(record.height_q8);
            right_chunk.search_summaries.push_back(record.search_summary);
        }
        current->chunk.logical_ids.resize(split_at);
        current->chunk.text_bytes.resize(split_at);
        current->chunk.heights_q8.resize(split_at);
        current->chunk.search_summaries.resize(split_at);

        if (!refresh(current)) {
            *error = "sequence aggregate overflow during chunk split";
            return false;
        }
        auto right_node = make_node(std::move(right_chunk));
        if (!right_node) {
            *error = "sequence aggregate overflow during chunk split";
            return false;
        }
        insert_front(&current->right, std::move(right_node));
        *slot = rebalance(std::move(*slot));
        return true;
    }

    bool insert_impl(std::unique_ptr<Node>* slot, std::uint64_t index, const SequenceRecord& record, std::string* error) {
        if (!*slot) {
            Chunk chunk;
            chunk.reserve(max_records_per_chunk);
            chunk.insert(0U, record);
            *slot = make_node(std::move(chunk));
            if (!*slot) {
                *error = "sequence aggregate overflow during insert";
                return false;
            }
            return true;
        }

        Node* node = slot->get();
        const std::uint64_t left_count = aggregate_of(node->left).record_count;
        const std::uint64_t chunk_count = static_cast<std::uint64_t>(node->chunk.size());
        bool success = false;
        if (index < left_count) {
            success = insert_impl(&node->left, index, record, error);
        } else if (index <= left_count + chunk_count) {
            const std::uint64_t local64 = index - left_count;
            if (local64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                *error = "sequence local index overflow";
                return false;
            }
            node->chunk.insert(static_cast<std::size_t>(local64), record);
            success = true;
        } else {
            success = insert_impl(&node->right, index - left_count - chunk_count, record, error);
        }
        if (!success) {
            return false;
        }
        if (!refresh(node)) {
            *error = "sequence aggregate overflow during insert";
            return false;
        }
        if (node->chunk.size() > max_records_per_chunk) {
            return split_chunk_node(slot, error);
        }
        *slot = rebalance(std::move(*slot));
        return true;
    }

    bool erase_impl(std::unique_ptr<Node>* slot, std::uint64_t index, SequenceRecord* erased, std::string* error) {
        Node* node = slot->get();
        if (node == nullptr) {
            *error = "sequence erase index is outside the sequence";
            return false;
        }
        const std::uint64_t left_count = aggregate_of(node->left).record_count;
        const std::uint64_t chunk_count = static_cast<std::uint64_t>(node->chunk.size());
        if (index < left_count) {
            if (!erase_impl(&node->left, index, erased, error)) {
                return false;
            }
        } else if (index >= left_count + chunk_count) {
            if (!erase_impl(&node->right, index - left_count - chunk_count, erased, error)) {
                return false;
            }
        } else {
            const std::size_t local = static_cast<std::size_t>(index - left_count);
            *erased = node->chunk.erase(local);
            if (node->chunk.size() == 0U) {
                *slot = join(std::move(node->left), std::move(node->right));
                return true;
            }
        }
        if (!refresh(slot->get())) {
            *error = "sequence aggregate overflow during erase";
            return false;
        }
        *slot = rebalance(std::move(*slot));
        return true;
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
