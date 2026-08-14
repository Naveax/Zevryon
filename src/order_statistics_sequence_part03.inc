bool ChunkedOrderStatisticsSequence::Snapshot::empty() const noexcept { return stats_.aggregate.record_count == 0U; }

bool ChunkedOrderStatisticsSequence::Snapshot::at(
    std::uint64_t record_index,
    SequencePosition* position,
    std::string* error) const {
    if (!impl_ || position == nullptr || error == nullptr || record_index >= stats_.aggregate.record_count) {
        if (error != nullptr) {
            *error = "invalid sequence access";
        }
        return false;
    }
    error->clear();
    return Impl::locate_by_records(impl_->root, record_index, position, error);
}

bool ChunkedOrderStatisticsSequence::Snapshot::locate_text_offset(
    std::uint64_t text_offset,
    SequencePosition* position,
    std::string* error) const {
    if (!impl_ || position == nullptr || error == nullptr || text_offset >= stats_.aggregate.text_bytes) {
        if (error != nullptr) {
            *error = "invalid sequence text offset";
        }
        return false;
    }
    error->clear();
    return Impl::locate_by_metric(impl_->root, text_offset, Impl::Metric::Text, position, error);
}

bool ChunkedOrderStatisticsSequence::Snapshot::locate_height_offset(
    std::uint64_t y_q8,
    SequencePosition* position,
    std::string* error) const {
    if (!impl_ || position == nullptr || error == nullptr || y_q8 >= stats_.aggregate.layout_height_q8) {
        if (error != nullptr) {
            *error = "invalid sequence height offset";
        }
        return false;
    }
    error->clear();
    return Impl::locate_by_metric(impl_->root, y_q8, Impl::Metric::Height, position, error);
}

bool ChunkedOrderStatisticsSequence::Snapshot::prefix(
    std::uint64_t record_count,
    SequenceAggregate* aggregate,
    std::string* error) const {
    if (!impl_ || aggregate == nullptr || error == nullptr || record_count > stats_.aggregate.record_count) {
        if (error != nullptr) {
            *error = "invalid sequence prefix";
        }
        return false;
    }
    error->clear();
    *aggregate = SequenceAggregate{};
    return Impl::prefix_impl(impl_->root.get(), record_count, aggregate, error);
}

ChunkedOrderStatisticsSequence::ChunkedOrderStatisticsSequence(std::uint32_t max_records_per_chunk) {
    const std::uint32_t max_chunk = std::clamp(max_records_per_chunk, 8U, 4096U);
    impl_ = std::make_shared<Impl>(max_chunk, nullptr);
    stats_ = Impl::make_stats(impl_->root, impl_->max_records_per_chunk);
}

ChunkedOrderStatisticsSequence::~ChunkedOrderStatisticsSequence() = default;
ChunkedOrderStatisticsSequence::ChunkedOrderStatisticsSequence(ChunkedOrderStatisticsSequence&&) noexcept = default;
ChunkedOrderStatisticsSequence& ChunkedOrderStatisticsSequence::operator=(ChunkedOrderStatisticsSequence&&) noexcept = default;

void ChunkedOrderStatisticsSequence::commit_root(std::shared_ptr<const Impl> next) noexcept {
    impl_ = std::move(next);
    stats_ = Impl::make_stats(impl_->root, impl_->max_records_per_chunk);
}

const SequenceStats& ChunkedOrderStatisticsSequence::stats() const noexcept { return stats_; }
bool ChunkedOrderStatisticsSequence::empty() const noexcept { return stats_.aggregate.record_count == 0U; }
ChunkedOrderStatisticsSequence::Snapshot ChunkedOrderStatisticsSequence::snapshot() const noexcept {
    return Snapshot(impl_, stats_);
}

bool ChunkedOrderStatisticsSequence::at(
    std::uint64_t record_index,
    SequencePosition* position,
    std::string* error) const {
    return snapshot().at(record_index, position, error);
}

bool ChunkedOrderStatisticsSequence::locate_text_offset(
    std::uint64_t text_offset,
    SequencePosition* position,
    std::string* error) const {
    return snapshot().locate_text_offset(text_offset, position, error);
}

bool ChunkedOrderStatisticsSequence::locate_height_offset(
    std::uint64_t y_q8,
    SequencePosition* position,
    std::string* error) const {
    return snapshot().locate_height_offset(y_q8, position, error);
}

bool ChunkedOrderStatisticsSequence::prefix(
    std::uint64_t record_count,
    SequenceAggregate* aggregate,
    std::string* error) const {
    return snapshot().prefix(record_count, aggregate, error);
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
    Impl::NodePtr root;
    if (!Impl::insert_impl(
            impl_->root,
            record_index,
            record,
            impl_->max_records_per_chunk,
            &root,
            error)) {
        return false;
    }
    commit_root(std::make_shared<Impl>(impl_->max_records_per_chunk, std::move(root)));
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
    Impl::NodePtr root;
    if (!Impl::erase_impl(impl_->root, record_index, erased, &root, error)) {
        return false;
    }
    commit_root(std::make_shared<Impl>(impl_->max_records_per_chunk, std::move(root)));
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
    Impl::NodePtr erased_root;
    if (!Impl::erase_impl(impl_->root, from_index, &record, &erased_root, error)) {
        return false;
    }
    Impl::NodePtr moved_root;
    if (!Impl::insert_impl(
            erased_root,
            to_index,
            record,
            impl_->max_records_per_chunk,
            &moved_root,
            error)) {
        return false;
    }
    commit_root(std::make_shared<Impl>(impl_->max_records_per_chunk, std::move(moved_root)));
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
    SequencePosition current;
    if (!at(record_index, &current, error)) {
        return false;
    }
    if (new_height_q8 > current.record.height_q8 &&
        stats_.aggregate.layout_height_q8 >
            std::numeric_limits<std::uint64_t>::max() - (new_height_q8 - current.record.height_q8)) {
        *error = "sequence aggregate overflow during height update";
        return false;
    }
    error->clear();
    Impl::NodePtr root;
    if (!Impl::update_height_impl(impl_->root, record_index, new_height_q8, old_height_q8, &root, error)) {
        return false;
    }
    commit_root(std::make_shared<Impl>(impl_->max_records_per_chunk, std::move(root)));
    return true;
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
    Impl::NodePtr root;
    if (!Impl::update_search_impl(impl_->root, record_index, new_summary, old_summary, &root, error)) {
        return false;
    }
    commit_root(std::make_shared<Impl>(impl_->max_records_per_chunk, std::move(root)));
    return true;
}

} // namespace zevryon::massivedoc
