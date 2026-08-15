        if (buffer_position_ >= buffer_size_) {
            if (!fill(error)) {
                return false;
            }
        }
        *byte = std::to_integer<unsigned char>(buffer_[buffer_position_++]);
        ++next_offset_;
        return true;
    }

    const BoundedPositionalReader* reader_{nullptr};
    std::uint64_t end_{0U};
    std::uint64_t next_offset_{0U};
    std::uint64_t expected_count_{0U};
    std::uint64_t block_count_{0U};
    std::uint64_t decoded_count_{0U};
    std::uint64_t current_{0U};
    std::vector<std::byte> buffer_;
    std::size_t buffer_position_{0U};
    std::size_t buffer_size_{0U};
    bool valid_{false};
};

} // namespace

bool TrigramIndexReader::visit_candidate_blocks(
    std::string_view query,
    const TrigramCandidateVisitor& visitor,
    const TrigramCancellationCheck& cancelled,
    TrigramQueryStats* stats,
    std::string* error) const {
    if (stats != nullptr) {
        *stats = {};
    }
    if (impl_ == nullptr || !impl_->opened) {
        *error = "trigram index is not open";
        return false;
    }
    if (query.size() < 3U) {
        for (std::uint64_t block = 0U; block < impl_->block_count; ++block) {
            if (cancellation_requested(cancelled)) {
                if (stats != nullptr) {
                    stats->cancelled = true;
                }
                *error = "trigram query cancelled";
                return false;
            }
            if (!visitor(block)) {
                return true;
            }
            if (stats != nullptr) {
                ++stats->candidate_blocks_emitted;
            }
        }
        return true;
    }

    std::vector<DirectoryEntry> selected;
    selected.reserve(kTrigramIntersectionStreams);
    for (std::size_t query_index = 2U; query_index < query.size(); ++query_index) {
        if ((query_index & 0x3ffU) == 0U && cancellation_requested(cancelled)) {
            if (stats != nullptr) {
                stats->cancelled = true;
            }
            *error = "trigram query cancelled";
            return false;
        }
        const std::uint32_t trigram = make_trigram(
            static_cast<unsigned char>(query[query_index - 2U]),
            static_cast<unsigned char>(query[query_index - 1U]),
            static_cast<unsigned char>(query[query_index]));
        bool duplicate = false;
        for (const auto& entry : selected) {
            if (entry.trigram == trigram) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        DirectoryEntry entry;
        bool found = false;
        if (!impl_->lookup(trigram, &entry, &found, error)) {
            return false;
        }
        if (!found) {
            return true;
        }
        if (selected.size() < kTrigramIntersectionStreams) {
            selected.push_back(entry);
            std::sort(selected.begin(), selected.end(), [](const DirectoryEntry& left, const DirectoryEntry& right) {
                if (left.block_count != right.block_count) {
                    return left.block_count < right.block_count;
                }
                return left.trigram < right.trigram;
            });
        } else if (entry.block_count < selected.back().block_count ||
                   (entry.block_count == selected.back().block_count && entry.trigram < selected.back().trigram)) {
            selected.back() = entry;
            std::sort(selected.begin(), selected.end(), [](const DirectoryEntry& left, const DirectoryEntry& right) {
                if (left.block_count != right.block_count) {
                    return left.block_count < right.block_count;
                }
                return left.trigram < right.trigram;
            });
        }
    }
    if (selected.empty()) {
        return true;
    }

    std::vector<std::unique_ptr<PostingCursor>> cursors;
    cursors.reserve(selected.size());
    for (const auto& entry : selected) {
        if (!impl_->validate_postings(entry, error)) {
            return false;
        }
        if (entry.postings_offset > impl_->postings_bytes ||
            entry.postings_bytes > impl_->postings_bytes - entry.postings_offset ||
            impl_->postings_offset > std::numeric_limits<std::uint64_t>::max() - entry.postings_offset) {
            *error = "trigram posting directory extent overflow";
            return false;
        }
        auto cursor = std::make_unique<PostingCursor>(
            impl_->index.get(),
            impl_->postings_offset + entry.postings_offset,
            entry.postings_bytes,
            entry.block_count,
            impl_->block_count,
            impl_->config.io_window_bytes);
        if (!cursor->initialize(error)) {
            return false;
        }
        cursors.push_back(std::move(cursor));
    }
    if (stats != nullptr) {
        stats->posting_streams_opened = static_cast<std::uint64_t>(cursors.size());
    }

    for (;;) {
        if (cancellation_requested(cancelled)) {
            if (stats != nullptr) {
                stats->cancelled = true;
            }
            *error = "trigram query cancelled";
            return false;
        }
        for (const auto& cursor : cursors) {
            if (!cursor->valid()) {
                return true;
            }
        }
        std::uint64_t target = 0U;
        for (const auto& cursor : cursors) {
            target = std::max(target, cursor->current());
        }
        bool all_equal = true;
        for (auto& cursor : cursors) {
            while (cursor->valid() && cursor->current() < target) {
                if (!cursor->advance(error)) {
                    return false;
                }
                if (stats != nullptr) {
                    ++stats->posting_blocks_advanced;
                }
            }
            if (!cursor->valid()) {
                return true;
            }
            if (cursor->current() != target) {
                all_equal = false;
                target = std::max(target, cursor->current());
            }
        }
        if (!all_equal) {
            continue;
        }
        if (stats != nullptr) {
            ++stats->bloom_blocks_checked;
        }
        bool bloom_allowed = false;
        if (!impl_->bloom_allows(target, query, &bloom_allowed, error)) {
            return false;
        }
        if (bloom_allowed) {
            if (!visitor(target)) {
                return true;
            }
            if (stats != nullptr) {
                ++stats->candidate_blocks_emitted;
            }
        }
        for (auto& cursor : cursors) {
            if (!cursor->advance(error)) {
                return false;
            }
            if (stats != nullptr) {
                ++stats->posting_blocks_advanced;
            }
        }
    }
}

bool TrigramIndexReader::is_open() const noexcept {
    return impl_ != nullptr && impl_->opened;
}

std::uint64_t TrigramIndexReader::block_count() const noexcept {
    return impl_ != nullptr ? impl_->block_count : 0U;
}

} // namespace zevryon::massivedoc
