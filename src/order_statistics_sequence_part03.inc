    std::string* error) const {
    if (aggregate == nullptr || error == nullptr || record_count > stats_.aggregate.record_count) {
        if (error != nullptr) {
            *error = "invalid sequence prefix";
        }
        return false;
    }
    error->clear();
    *aggregate = SequenceAggregate{};
    return Impl::prefix_impl(impl_->root.get(), record_count, aggregate, error);
}

bool ChunkedOrderStatisticsSequence::insert(
    std::uint64_t record_index,
    SequenceRecord record,
    std::string* error) {
    if (error == nullptr || record_index > stats_.aggregate.record_count || record.height_q8 == 0U) {
        if (error != nullptr) {
            *error = "invalid sequence insert";
        }
        return false;
    }
    std::uint64_t ignored = 0U;
    if (!checked_add(stats_.aggregate.record_count, 1U, &ignored) ||
        !checked_add(stats_.aggregate.text_bytes, record.text_bytes, &ignored) ||
        !checked_add(stats_.aggregate.layout_height_q8, record.height_q8, &ignored)) {
        *error = "sequence aggregate overflow during insert";
        return false;
    }
    error->clear();
    if (!impl_->insert_impl(&impl_->root, record_index, record, error)) {
        return false;
    }
    refresh_stats();
    return true;
}

bool ChunkedOrderStatisticsSequence::erase(
    std::uint64_t record_index,
    SequenceRecord* erased,
    std::string* error) {
    if (erased == nullptr || error == nullptr || record_index >= stats_.aggregate.record_count) {
        if (error != nullptr) {
            *error = "invalid sequence erase";
        }
        return false;
    }
    error->clear();
    if (!impl_->erase_impl(&impl_->root, record_index, erased, error)) {
        return false;
    }
    refresh_stats();
    return true;
}

bool ChunkedOrderStatisticsSequence::move(
    std::uint64_t from_index,
    std::uint64_t to_index,
    std::string* error) {
    if (error == nullptr || from_index >= stats_.aggregate.record_count || to_index >= stats_.aggregate.record_count) {
        if (error != nullptr) {
            *error = "invalid sequence move";
        }
        return false;
    }
    error->clear();
    if (from_index == to_index) {
        return true;
    }
    SequenceRecord record;
    if (!impl_->erase_impl(&impl_->root, from_index, &record, error)) {
        return false;
    }
    if (!impl_->insert_impl(&impl_->root, to_index, record, error)) {
        // This path is expected to be unreachable for a previously admitted record. Restore best-effort.
        std::string restore_error;
        static_cast<void>(impl_->insert_impl(&impl_->root, std::min(from_index, Impl::aggregate_of(impl_->root).record_count), record, &restore_error));
        refresh_stats();
        return false;
    }
    refresh_stats();
    return true;
}

bool ChunkedOrderStatisticsSequence::update_height(
    std::uint64_t record_index,
    std::uint32_t new_height_q8,
    std::uint32_t* old_height_q8,
    std::string* error) {
    if (old_height_q8 == nullptr || error == nullptr || new_height_q8 == 0U || record_index >= stats_.aggregate.record_count) {
        if (error != nullptr) {
            *error = "invalid sequence height update";
        }
        return false;
    }
    error->clear();
    std::vector<Impl::Node*> path;
    Impl::Node* node = impl_->root.get();
    std::uint64_t index = record_index;
    while (node != nullptr) {
        path.push_back(node);
        const std::uint64_t left_count = node->left ? node->left->aggregate.record_count : 0U;
        if (index < left_count) {
            node = node->left.get();
            continue;
        }
        const std::uint64_t local_count = static_cast<std::uint64_t>(node->chunk.size());
        if (index < left_count + local_count) {
            const std::size_t local = static_cast<std::size_t>(index - left_count);
            const std::uint32_t old = node->chunk.heights_q8[local];
            if (new_height_q8 > old && stats_.aggregate.layout_height_q8 > std::numeric_limits<std::uint64_t>::max() - (new_height_q8 - old)) {
                *error = "sequence aggregate overflow during height update";
                return false;
            }
            node->chunk.heights_q8[local] = new_height_q8;
            for (auto iterator = path.rbegin(); iterator != path.rend(); ++iterator) {
                if (!Impl::refresh(*iterator)) {
                    node->chunk.heights_q8[local] = old;
                    for (auto rollback = path.rbegin(); rollback != path.rend(); ++rollback) {
                        static_cast<void>(Impl::refresh(*rollback));
                    }
                    *error = "sequence aggregate overflow during height update";
                    return false;
                }
            }
            *old_height_q8 = old;
            refresh_stats();
            return true;
        }
        index -= left_count + local_count;
        node = node->right.get();
    }
    *error = "sequence height update index is outside the sequence";
    return false;
}

bool ChunkedOrderStatisticsSequence::update_search_summary(
    std::uint64_t record_index,
    std::uint64_t new_summary,
    std::uint64_t* old_summary,
    std::string* error) {
    if (old_summary == nullptr || error == nullptr || record_index >= stats_.aggregate.record_count) {
        if (error != nullptr) {
            *error = "invalid sequence search-summary update";
        }
        return false;
    }
    error->clear();
    std::vector<Impl::Node*> path;
    Impl::Node* node = impl_->root.get();
    std::uint64_t index = record_index;
    while (node != nullptr) {
        path.push_back(node);
        const std::uint64_t left_count = node->left ? node->left->aggregate.record_count : 0U;
        if (index < left_count) {
            node = node->left.get();
            continue;
        }
        const std::uint64_t local_count = static_cast<std::uint64_t>(node->chunk.size());
        if (index < left_count + local_count) {
            const std::size_t local = static_cast<std::size_t>(index - left_count);
            *old_summary = node->chunk.search_summaries[local];
            node->chunk.search_summaries[local] = new_summary;
            for (auto iterator = path.rbegin(); iterator != path.rend(); ++iterator) {
                static_cast<void>(Impl::refresh(*iterator));
            }
            refresh_stats();
            return true;
        }
        index -= left_count + local_count;
        node = node->right.get();
    }
    *error = "sequence search-summary update index is outside the sequence";
    return false;
}

} // namespace zevryon::massivedoc
