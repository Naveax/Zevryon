            }
            current_trigram = entry.trigram;
            group_offset = postings_offset;
            group_bytes = 0U;
            group_count = 0U;
            previous_block = 0U;
            have_previous_block = false;
        }
        if (have_previous_block && entry.block <= previous_block) {
            *error = "trigram sorted run is not strictly ordered by block";
            return false;
        }
        const std::uint64_t delta = have_previous_block ? entry.block - previous_block : entry.block;
        std::vector<std::byte> encoded;
        encoded.reserve(10U);
        encode_varint(delta, &encoded);
        if (!write_all(postings, encoded)) {
            *error = "failed to write trigram posting delta";
            return false;
        }
        postings_offset += static_cast<std::uint64_t>(encoded.size());
        group_bytes += static_cast<std::uint64_t>(encoded.size());
        ++group_count;
        ++posting_pairs;
        previous_block = entry.block;
        have_previous_block = true;
    }
    if (!flush_group()) {
        return false;
    }
    directory.flush();
    postings.flush();
    if (!directory || !postings) {
        *error = "failed to flush trigram directory/postings";
        return false;
    }
    directory.close();
    postings.close();

    if (distinct > (std::numeric_limits<std::uint64_t>::max() - kHeaderBytes) / kDirectoryEntryBytes) {
        *error = "trigram directory size overflow";
        return false;
    }
    const std::uint64_t directory_offset = kHeaderBytes;
    const std::uint64_t postings_file_offset =
        directory_offset + distinct * static_cast<std::uint64_t>(kDirectoryEntryBytes);

    std::vector<std::byte> header;
    header.reserve(kHeaderBytes);
    for (const char character : kIndexMagic) {
        header.push_back(static_cast<std::byte>(character));
    }
    append_le(header, kIndexVersion);
    append_le(header, static_cast<std::uint32_t>(kHeaderBytes));
    append_le(header, static_cast<std::uint32_t>(kTrigramBloomBytes));
    append_le(header, static_cast<std::uint32_t>(kTrigramIntersectionStreams));
    append_le(header, total_blocks);
    append_le(header, distinct);
    append_le(header, posting_pairs);
    append_le(header, directory_offset);
    append_le(header, postings_file_offset);
    append_le(header, postings_offset);
    if (header.size() != 72U) {
        *error = "internal trigram header prefix size mismatch";
        return false;
    }
    const std::uint32_t header_crc = crc32_update(0U, header);
    append_le(header, header_crc);
    append_le(header, std::uint32_t{0U});
    if (header.size() != kHeaderBytes) {
        *error = "internal trigram header size mismatch";
        return false;
    }

    const auto index_temp = impl_->temp_root / "search.tri.tmp";
    {
        std::ofstream output(index_temp, std::ios::binary | std::ios::trunc);
        if (!output) {
            *error = "cannot create trigram index temporary output";
            return false;
        }
        if (!write_all(output, header) ||
            !copy_file_bounded(directory_path, output, error) ||
            !copy_file_bounded(postings_path, output, error)) {
            if (error->empty()) {
                *error = "failed to assemble trigram index";
            }
            return false;
        }
        output.flush();
        if (!output) {
            *error = "failed to flush trigram index temporary output";
            return false;
        }
    }

    std::error_code fs_error;
    std::filesystem::remove(impl_->root / "search.tri", fs_error);
    fs_error.clear();
    std::filesystem::rename(index_temp, impl_->root / "search.tri", fs_error);
    if (fs_error) {
        *error = "cannot publish trigram index: " + fs_error.message();
        return false;
    }
    fs_error.clear();
    std::filesystem::remove(impl_->root / "search.blm", fs_error);
    fs_error.clear();
    std::filesystem::rename(impl_->bloom_temp_path, impl_->root / "search.blm", fs_error);
    if (fs_error) {
        *error = "cannot publish trigram Bloom summaries: " + fs_error.message();
        return false;
    }

    const std::uint64_t expected_bloom_bytes =
        total_blocks * static_cast<std::uint64_t>(kTrigramBloomBytes);
    std::error_code size_error;
    const std::uint64_t index_size = std::filesystem::file_size(impl_->root / "search.tri", size_error);
    if (size_error) {
        *error = "cannot stat published trigram index";
        return false;
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

    bool bloom_allows(std::uint64_t block, std::string_view query, std::string* error) const {
        if (block >= block_count) {
            *error = "trigram Bloom block out of range";
            return false;
        }
        if (block > std::numeric_limits<std::uint64_t>::max() / kTrigramBloomBytes) {
            *error = "trigram Bloom offset overflow";
            return false;
        }
