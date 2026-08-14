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
