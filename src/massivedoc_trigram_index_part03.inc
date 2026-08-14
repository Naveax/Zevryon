    std::uint64_t group_bytes = 0U;
    std::uint64_t group_count = 0U;
    std::uint32_t group_crc = 0U;
    std::uint64_t previous_block = 0U;
    bool have_previous_block = false;

    auto flush_group = [&]() -> bool {
        if (!current_trigram) {
            return true;
        }
        const DirectoryEntry entry{
            *current_trigram,
            group_crc,
            group_offset,
            group_bytes,
            group_count,
        };
        const auto bytes = serialize_directory(entry);
        if (!write_all(directory, bytes)) {
            *error = "failed to write trigram directory entry";
            return false;
        }
        ++distinct;
        return true;
    };

    for (;;) {
        if ((posting_pairs & 0xfffU) == 0U && cancellation_requested(cancelled)) {
            *error = "trigram index build cancelled";
            return false;
        }
        SpoolEntry entry;
        bool eof = false;
        if (!read_spool_entry(sorted, &entry, &eof, error)) {
            return false;
        }
        if (eof) {
            break;
        }
        if (!current_trigram || *current_trigram != entry.trigram) {
            if (!flush_group()) {
                return false;
            }
            current_trigram = entry.trigram;
            group_offset = postings_offset;
            group_bytes = 0U;
            group_count = 0U;
            group_crc = 0U;
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
        group_crc = crc32_update(group_crc, encoded);
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
    append_le(header, static_cast<std::uint32_t>(kBloomFrameBytes));
    append_le(header, static_cast<std::uint32_t>(kTrigramIntersectionStreams));
    append_le(header, total_blocks);
    append_le(header, distinct);
    append_le(header, posting_pairs);
    append_le(header, directory_offset);
    append_le(header, postings_file_offset);
    append_le(header, postings_offset);
    for (const std::uint8_t byte : source_identity) {
        header.push_back(static_cast<std::byte>(byte));
    }
    if (header.size() != 104U) {
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
    if (total_blocks >
        (std::numeric_limits<std::uint64_t>::max() - kBloomHeaderBytes) / kBloomFrameBytes) {
        *error = "trigram Bloom size overflow";
        return false;
    }
    std::vector<std::byte> bloom_header;
    bloom_header.reserve(kBloomHeaderBytes);
    for (const char character : kBloomMagic) {
        bloom_header.push_back(static_cast<std::byte>(character));
    }
    append_le(bloom_header, kBloomVersion);
    append_le(bloom_header, static_cast<std::uint32_t>(kBloomHeaderBytes));
    append_le(bloom_header, static_cast<std::uint32_t>(kBloomFrameBytes));
    append_le(bloom_header, std::uint32_t{0U});
    append_le(bloom_header, total_blocks);
    for (const std::uint8_t byte : source_identity) {
        bloom_header.push_back(static_cast<std::byte>(byte));
    }
    if (bloom_header.size() != 64U) {
        *error = "internal trigram Bloom header prefix size mismatch";
        return false;
    }
    append_le(bloom_header, crc32_update(0U, bloom_header));
    append_le(bloom_header, std::uint32_t{0U});
    append_le(bloom_header, std::uint32_t{0U});
    append_le(bloom_header, std::uint32_t{0U});
    if (bloom_header.size() != kBloomHeaderBytes) {
        *error = "internal trigram Bloom header size mismatch";
        return false;
    }
    const auto bloom_output_temp = impl_->temp_root / "search.blm.final.tmp";
    {
        std::ofstream output(bloom_output_temp, std::ios::binary | std::ios::trunc);
        if (!output || !write_all(output, bloom_header) ||
            !copy_file_bounded(impl_->bloom_temp_path, output, error)) {
            if (error->empty()) {
                *error = "failed to assemble trigram Bloom summaries";
            }
            return false;
        }
        output.flush();
        if (!output) {
            *error = "failed to flush trigram Bloom summary output";
            return false;
        }
    }
    fs_error.clear();
    std::filesystem::remove(impl_->root / "search.blm", fs_error);
    fs_error.clear();
    std::filesystem::rename(bloom_output_temp, impl_->root / "search.blm", fs_error);
    if (fs_error) {
        *error = "cannot publish trigram Bloom summaries: " + fs_error.message();
        return false;
    }

    const std::uint64_t expected_bloom_bytes = kBloomHeaderBytes +
        total_blocks * static_cast<std::uint64_t>(kBloomFrameBytes);
    std::error_code size_error;
    const std::uint64_t index_size = std::filesystem::file_size(impl_->root / "search.tri", size_error);
    if (size_error) {
        *error = "cannot stat published trigram index";
        return false;
