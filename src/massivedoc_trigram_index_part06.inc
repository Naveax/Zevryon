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
