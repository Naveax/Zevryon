    if (count == 0U) {
        return true;
    }
    std::ifstream stream(records_path, std::ios::binary);
    const std::uint64_t offset = first_record * kRecordDescriptorBytes;
    if (!stream || !seek_stream(stream, offset)) {
        *error = "cannot seek record index";
        return false;
    }
    std::vector<std::byte> bytes(count * kRecordDescriptorBytes);
    if (!read_exact(stream, bytes)) {
        *error = "cannot read record descriptor block";
        return false;
    }
    descriptors->reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto descriptor = parse_record_descriptor(std::span<const std::byte>(
            bytes.data() + index * kRecordDescriptorBytes, kRecordDescriptorBytes));
        if (!descriptor) {
            *error = "invalid record descriptor";
            return false;
        }
        descriptors->push_back(*descriptor);
    }
    return true;
}

} // namespace

bool build_compact_arena(
    const std::filesystem::path& store_root,
    ArenaConfig config,
    ArenaStats* stats,
    std::string* error) {
    if (stats == nullptr || error == nullptr) {
        return false;
    }
    error->clear();
    if (config.records_per_block == 0U || config.estimated_bytes_per_line == 0U || config.line_height_q8 == 0U) {
        *error = "invalid arena configuration";
        return false;
    }

    const auto records_path = store_root / "records.idx";
    std::error_code size_error;
    const std::uint64_t records_size = std::filesystem::file_size(records_path, size_error);
    if (size_error || records_size % kRecordDescriptorBytes != 0U) {
        *error = "invalid records index size";
        return false;
    }
    const std::uint64_t logical_records = records_size / kRecordDescriptorBytes;
    if (logical_records == 0U) {
        *error = "empty store";
        return false;
    }

    ArenaStats output;
    output.logical_records = logical_records;
    output.config = config;

    std::ifstream manifest(store_root / "manifest.zmd", std::ios::binary);
    if (!manifest) {
        *error = "missing store manifest";
        return false;
    }
    std::array<std::byte, 128U> manifest_bytes{};
    manifest.read(reinterpret_cast<char*>(manifest_bytes.data()), static_cast<std::streamsize>(manifest_bytes.size()));
    const std::size_t manifest_count = static_cast<std::size_t>(manifest.gcount());
    if (manifest_count < 40U) {
        *error = "short store manifest";
        return false;
    }
    // The M1 manifest stores logical records at byte 40 and logical nodes at byte 48.
    const auto manifest_records = read_le<std::uint64_t>(std::span<const std::byte>(manifest_bytes.data(), manifest_count), 40U);
    const auto logical_nodes = read_le<std::uint64_t>(std::span<const std::byte>(manifest_bytes.data(), manifest_count), 48U);
    if (!manifest_records || *manifest_records != logical_records || !logical_nodes) {
        *error = "store manifest record/node mismatch";
        return false;
    }
    output.logical_nodes = *logical_nodes;

    const auto arena_root = store_root / "arena";
    std::error_code fs_error;
    std::filesystem::remove_all(arena_root, fs_error);
    fs_error.clear();
    std::filesystem::create_directories(arena_root, fs_error);
    if (fs_error) {
        *error = "cannot create arena directory";
        return false;
    }

    std::ifstream records(records_path, std::ios::binary);
    std::ofstream heights(arena_root / "record-heights.idx", std::ios::binary | std::ios::trunc);
    std::ofstream blocks(arena_root / "height-blocks.idx", std::ios::binary | std::ios::trunc);
    if (!records || !heights || !blocks) {
        *error = "cannot open arena files";
        return false;
    }

    constexpr std::size_t kBatchRecords = 4096U;
    std::vector<std::byte> descriptor_bytes(kBatchRecords * kRecordDescriptorBytes);
    std::vector<std::byte> height_bytes;
    height_bytes.reserve(kBatchRecords * kHeightEntryBytes);
    std::uint64_t processed = 0;
    std::uint64_t block_height = 0;
    std::uint32_t records_in_block = 0;

    while (processed < logical_records) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(kBatchRecords, logical_records - processed));
        const std::size_t bytes_to_read = count * kRecordDescriptorBytes;
        if (!read_exact(records, std::span<std::byte>(descriptor_bytes.data(), bytes_to_read))) {
            *error = "cannot stream record index";
            return false;
        }
        height_bytes.clear();
        for (std::size_t index = 0; index < count; ++index) {
            const auto descriptor = parse_record_descriptor(std::span<const std::byte>(
                descriptor_bytes.data() + index * kRecordDescriptorBytes, kRecordDescriptorBytes));
            if (!descriptor) {
                *error = "invalid record descriptor during arena build";
                return false;
            }
            const std::uint32_t height_q8 = estimate_height_q8(descriptor->length, config);
            append_le(height_bytes, height_q8);
            block_height += height_q8;
            output.total_height_q8 += height_q8;
            ++records_in_block;
            if (records_in_block == config.records_per_block) {
                std::vector<std::byte> block_bytes;
                block_bytes.reserve(kBlockEntryBytes);
                append_le(block_bytes, block_height);
                if (!write_all(blocks, block_bytes)) {
                    *error = "cannot write height block";
                    return false;
                }
                ++output.block_count;
                block_height = 0;
                records_in_block = 0;
            }
        }
        if (!write_all(heights, height_bytes)) {
            *error = "cannot write record heights";
            return false;
        }
        processed += count;
    }
    if (records_in_block != 0U) {
        std::vector<std::byte> block_bytes;
        block_bytes.reserve(kBlockEntryBytes);
        append_le(block_bytes, block_height);
        if (!write_all(blocks, block_bytes)) {
            *error = "cannot write final height block";
            return false;
        }
        ++output.block_count;
    }
    heights.flush();
    blocks.flush();

    output.physical_bytes = kArenaHeaderBytes + logical_records * kHeightEntryBytes + output.block_count * kBlockEntryBytes;
    const auto header_bytes = serialize_header(output);
