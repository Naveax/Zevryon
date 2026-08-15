    }
    const std::uint64_t bloom_size = std::filesystem::file_size(impl_->root / "search.blm", size_error);
    if (size_error || bloom_size != expected_bloom_bytes) {
        *error = "published trigram Bloom size mismatch";
        return false;
    }

    if (stats != nullptr) {
        stats->block_count = total_blocks;
        stats->posting_pairs = posting_pairs;
        stats->distinct_trigrams = distinct;
        stats->spool_bytes = impl_->spool_entries * static_cast<std::uint64_t>(kSpoolEntryBytes);
        stats->index_bytes = index_size;
        stats->bloom_bytes = bloom_size;
        stats->merge_passes = pass;
    }
    impl_->finished = true;
    std::filesystem::remove_all(impl_->temp_root, fs_error);
    return true;
}

struct TrigramIndexReader::Impl {
    std::filesystem::path root;
    TrigramIndexConfig config;
    std::unique_ptr<BoundedPositionalReader> index;
    std::unique_ptr<BoundedPositionalReader> bloom;
    std::uint64_t block_count{0U};
    std::uint64_t distinct_trigrams{0U};
    std::uint64_t posting_pairs{0U};
    std::uint64_t directory_offset{0U};
    std::uint64_t postings_offset{0U};
    std::uint64_t postings_bytes{0U};
    bool opened{false};

    bool read_directory(std::uint64_t position, DirectoryEntry* entry, std::string* error) const {
        if (position >= distinct_trigrams) {
            *error = "trigram directory position out of range";
            return false;
        }
        if (position > (std::numeric_limits<std::uint64_t>::max() - directory_offset) / kDirectoryEntryBytes) {
            *error = "trigram directory offset overflow";
            return false;
        }
        const std::uint64_t offset = directory_offset +
            position * static_cast<std::uint64_t>(kDirectoryEntryBytes);
        std::array<std::byte, kDirectoryEntryBytes> bytes{};
        if (!index->read_exact_at(offset, bytes, error)) {
            *error = "cannot read trigram directory: " + *error;
            return false;
        }
        const auto parsed = parse_directory(bytes);
        if (!parsed) {
            *error = "invalid trigram directory entry";
            return false;
        }
        if (parsed->block_count == 0U || parsed->postings_bytes == 0U ||
            parsed->postings_offset > postings_bytes ||
            parsed->postings_bytes > postings_bytes - parsed->postings_offset) {
            *error = "invalid trigram directory entry extent";
            return false;
        }
        *entry = *parsed;
        return true;
    }

    bool lookup(std::uint32_t trigram, DirectoryEntry* entry, bool* found, std::string* error) const {
        std::uint64_t low = 0U;
        std::uint64_t high = distinct_trigrams;
        while (low < high) {
            const std::uint64_t middle = low + (high - low) / 2U;
            DirectoryEntry candidate;
            if (!read_directory(middle, &candidate, error)) {
                return false;
            }
            if (candidate.trigram < trigram) {
                low = middle + 1U;
            } else {
                high = middle;
            }
        }
        if (low >= distinct_trigrams) {
            *found = false;
            return true;
        }
        DirectoryEntry candidate;
        if (!read_directory(low, &candidate, error)) {
            return false;
        }
        if (candidate.trigram != trigram) {
            *found = false;
            return true;
        }
        *entry = candidate;
        *found = true;
        return true;
    }

    bool validate_postings(const DirectoryEntry& entry, std::string* error) const {
        if (entry.postings_offset > postings_bytes ||
            entry.postings_bytes > postings_bytes - entry.postings_offset ||
            postings_offset > std::numeric_limits<std::uint64_t>::max() - entry.postings_offset) {
            *error = "trigram posting directory extent overflow";
            return false;
        }
        const std::size_t window = std::max<std::size_t>(
            1U, std::min<std::size_t>(config.io_window_bytes, 64U * 1024U));
        std::vector<std::byte> buffer(window);
        std::uint64_t offset = postings_offset + entry.postings_offset;
        std::uint64_t remaining = entry.postings_bytes;
        std::uint32_t crc = 0U;
        while (remaining != 0U) {
            const std::size_t amount = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, buffer.size()));
            if (!index->read_exact_at(
                    offset, std::span<std::byte>(buffer.data(), amount), error)) {
                *error = "cannot validate trigram posting stream: " + *error;
                return false;
            }
            crc = crc32_update(crc, std::span<const std::byte>(buffer.data(), amount));
            offset += static_cast<std::uint64_t>(amount);
            remaining -= static_cast<std::uint64_t>(amount);
        }
        if (crc != entry.postings_crc32) {
            *error = "trigram posting checksum mismatch";
            return false;
        }
        return true;
    }

    bool bloom_allows(
        std::uint64_t block,
        std::string_view query,
        bool* allowed,
        std::string* error) const {
        *allowed = false;
        if (block >= block_count) {
            *error = "trigram Bloom block out of range";
            return false;
        }
        if (block > (std::numeric_limits<std::uint64_t>::max() - kBloomHeaderBytes) / kBloomFrameBytes) {
            *error = "trigram Bloom offset overflow";
            return false;
        }
        std::array<std::byte, kBloomFrameBytes> frame{};
        if (!bloom->read_exact_at(
                kBloomHeaderBytes + block * static_cast<std::uint64_t>(kBloomFrameBytes),
                frame,
                error)) {
            *error = "cannot read trigram Bloom summary: " + *error;
            return false;
        }
        const auto stored_crc = read_le<std::uint32_t>(frame, kTrigramBloomBytes);
        if (!stored_crc || crc32_update(0U, std::span<const std::byte>(frame.data(), kTrigramBloomBytes)) != *stored_crc) {
            *error = "trigram Bloom checksum mismatch";
            return false;
        }
        const auto bytes = std::span<const std::byte>(frame.data(), kTrigramBloomBytes);
        if (query.size() < 3U) {
            *allowed = true;
            return true;
        }
        for (std::size_t index_query = 2U; index_query < query.size(); ++index_query) {
            const auto trigram = make_trigram(
                static_cast<unsigned char>(query[index_query - 2U]),
                static_cast<unsigned char>(query[index_query - 1U]),
                static_cast<unsigned char>(query[index_query]));
            if (!bloom_has(bytes, trigram)) {
                return true;
            }
        }
        *allowed = true;
        return true;
    }
};

TrigramIndexReader::TrigramIndexReader(
    std::filesystem::path root,
    TrigramIndexConfig config)
    : impl_(new Impl{}) {
    impl_->root = std::move(root);
    impl_->config = config;
    if (impl_->config.io_window_bytes == 0U) {
        impl_->config.io_window_bytes = 64U * 1024U;
    }
}

TrigramIndexReader::~TrigramIndexReader() {
    delete impl_;
}

bool TrigramIndexReader::open(
    std::span<const std::uint8_t, kTrigramSourceIdentityBytes> expected_source_identity,
    std::string* error) {
    impl_->opened = false;
    impl_->index = std::make_unique<BoundedPositionalReader>(
        impl_->root / "search.tri",
        impl_->config.io_window_bytes);
    impl_->bloom = std::make_unique<BoundedPositionalReader>(
        impl_->root / "search.blm",
        impl_->config.io_window_bytes);
    if (!impl_->index->open(error)) {
        *error = "cannot open trigram index: " + *error;
        return false;
    }
    if (!impl_->bloom->open(error)) {
        *error = "cannot open trigram Bloom summaries: " + *error;
        return false;
    }
    if (impl_->index->file_size() < kHeaderBytes) {
        *error = "trigram index is shorter than its header";
        return false;
    }
    std::array<std::byte, kHeaderBytes> header{};
