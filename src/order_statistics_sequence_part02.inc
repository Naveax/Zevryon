        }
        if (node->left) {
            if (!combine(*output, node->left->aggregate, SequenceAggregate{}, output)) {
                *error = "sequence prefix aggregate overflow";
                return false;
            }
        }
        std::uint64_t remaining = count - left_count;
        const std::uint64_t local_count = std::min<std::uint64_t>(remaining, node->chunk.size());
        for (std::uint64_t local = 0U; local < local_count; ++local) {
            const std::size_t i = static_cast<std::size_t>(local);
            if (!checked_add(output->record_count, 1U, &output->record_count) ||
                !checked_add(output->text_bytes, node->chunk.text_bytes[i], &output->text_bytes) ||
                !checked_add(output->layout_height_q8, node->chunk.heights_q8[i], &output->layout_height_q8)) {
                *error = "sequence prefix aggregate overflow";
                return false;
            }
            output->search_summary |= node->chunk.search_summaries[i];
        }
        remaining -= local_count;
        if (remaining == 0U) {
            return true;
        }
        return prefix_impl(node->right.get(), remaining, output, error);
    }

    bool locate_by_records(std::uint64_t index, SequencePosition* position, std::string* error) const noexcept {
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
            const std::uint64_t local_limit = record_prefix + static_cast<std::uint64_t>(node->chunk.size());
            if (index < local_limit) {
                const std::size_t local = static_cast<std::size_t>(index - record_prefix);
                for (std::size_t i = 0U; i < local; ++i) {
                    if (!checked_add(text_prefix, node->chunk.text_bytes[i], &text_prefix) ||
                        !checked_add(height_prefix, node->chunk.heights_q8[i], &height_prefix)) {
                        *error = "sequence prefix overflow";
                        return false;
                    }
                }
                *position = SequencePosition{index, text_prefix, height_prefix, node->chunk.at(local)};
                return true;
            }
            for (std::size_t i = 0U; i < node->chunk.size(); ++i) {
                if (!checked_add(text_prefix, node->chunk.text_bytes[i], &text_prefix) ||
                    !checked_add(height_prefix, node->chunk.heights_q8[i], &height_prefix)) {
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

    bool locate_by_metric(std::uint64_t offset, Metric metric, SequencePosition* position, std::string* error) const noexcept {
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
            for (std::size_t local = 0U; local < node->chunk.size(); ++local) {
                const std::uint64_t width = metric == Metric::Text ? node->chunk.text_bytes[local] : node->chunk.heights_q8[local];
                const std::uint64_t current_metric = metric == Metric::Text ? text_prefix : height_prefix;
                if (width != 0U && offset >= current_metric && offset - current_metric < width) {
                    *position = SequencePosition{record_prefix, text_prefix, height_prefix, node->chunk.at(local)};
                    return true;
                }
                if (!checked_add(record_prefix, 1U, &record_prefix) ||
                    !checked_add(text_prefix, node->chunk.text_bytes[local], &text_prefix) ||
                    !checked_add(height_prefix, node->chunk.heights_q8[local], &height_prefix)) {
                    *error = "sequence prefix overflow";
                    return false;
                }
            }
            node = node->right.get();
        }
        *error = metric == Metric::Text ? "sequence text offset is outside the sequence" : "sequence height offset is outside the sequence";
        return false;
    }
};

ChunkedOrderStatisticsSequence::ChunkedOrderStatisticsSequence(std::uint32_t max_records_per_chunk)
    : impl_(std::make_unique<Impl>(std::clamp(max_records_per_chunk, 8U, 4096U))) {
    stats_.max_records_per_chunk = impl_->max_records_per_chunk;
}

ChunkedOrderStatisticsSequence::~ChunkedOrderStatisticsSequence() = default;
ChunkedOrderStatisticsSequence::ChunkedOrderStatisticsSequence(ChunkedOrderStatisticsSequence&&) noexcept = default;
ChunkedOrderStatisticsSequence& ChunkedOrderStatisticsSequence::operator=(ChunkedOrderStatisticsSequence&&) noexcept = default;

void ChunkedOrderStatisticsSequence::refresh_stats() noexcept {
    stats_.aggregate = impl_->root ? impl_->root->aggregate : SequenceAggregate{};
    stats_.chunk_count = impl_->root ? impl_->root->chunk_count : 0U;
    stats_.tree_height = impl_->root ? impl_->root->tree_height : 0U;
    stats_.max_records_per_chunk = impl_->max_records_per_chunk;
}

const SequenceStats& ChunkedOrderStatisticsSequence::stats() const noexcept { return stats_; }
bool ChunkedOrderStatisticsSequence::empty() const noexcept { return stats_.aggregate.record_count == 0U; }

bool ChunkedOrderStatisticsSequence::at(
    std::uint64_t record_index,
    SequencePosition* position,
    std::string* error) const {
    if (position == nullptr || error == nullptr || record_index >= stats_.aggregate.record_count) {
        if (error != nullptr) {
            *error = "invalid sequence access";
        }
        return false;
    }
    error->clear();
    return impl_->locate_by_records(record_index, position, error);
}

bool ChunkedOrderStatisticsSequence::locate_text_offset(
    std::uint64_t text_offset,
    SequencePosition* position,
    std::string* error) const {
    if (position == nullptr || error == nullptr || text_offset >= stats_.aggregate.text_bytes) {
        if (error != nullptr) {
            *error = "invalid sequence text offset";
        }
        return false;
    }
    error->clear();
    return impl_->locate_by_metric(text_offset, Impl::Metric::Text, position, error);
}

bool ChunkedOrderStatisticsSequence::locate_height_offset(
    std::uint64_t y_q8,
    SequencePosition* position,
    std::string* error) const {
    if (position == nullptr || error == nullptr || y_q8 >= stats_.aggregate.layout_height_q8) {
        if (error != nullptr) {
            *error = "invalid sequence height offset";
        }
        return false;
    }
    error->clear();
    return impl_->locate_by_metric(y_q8, Impl::Metric::Height, position, error);
}

bool ChunkedOrderStatisticsSequence::prefix(
    std::uint64_t record_count,
    SequenceAggregate* aggregate,
