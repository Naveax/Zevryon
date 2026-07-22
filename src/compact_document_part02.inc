    if (header_bytes.size() != kArenaHeaderBytes) {
        *error = "internal arena header size mismatch";
        return false;
    }
    std::ofstream header(arena_root / "arena.meta", std::ios::binary | std::ios::trunc);
    if (!header || !write_all(header, header_bytes)) {
        *error = "cannot write arena header";
        return false;
    }
    header.flush();
    const std::uint64_t measured_size = directory_size(arena_root);
    if (measured_size != output.physical_bytes) {
        *error = "arena physical size mismatch";
        return false;
    }
    *stats = output;
    return true;
}

CompactArenaReader::CompactArenaReader(const std::filesystem::path& store_root) : root_(store_root) {}

bool CompactArenaReader::open(std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    const auto arena_root = root_ / "arena";
    std::ifstream header(arena_root / "arena.meta", std::ios::binary);
    std::array<std::byte, kArenaHeaderBytes> header_bytes{};
    if (!header || !read_exact(header, header_bytes) || !parse_header(header_bytes, &stats_)) {
        *error = "invalid arena header";
        return false;
    }

    std::error_code size_error;
    const std::uint64_t heights_size = std::filesystem::file_size(arena_root / "record-heights.idx", size_error);
    if (size_error || heights_size != stats_.logical_records * kHeightEntryBytes) {
        *error = "record height index size mismatch";
        return false;
    }
    const std::uint64_t blocks_size = std::filesystem::file_size(arena_root / "height-blocks.idx", size_error);
    if (size_error || blocks_size != stats_.block_count * kBlockEntryBytes) {
        *error = "height block index size mismatch";
        return false;
    }

    std::ifstream blocks(arena_root / "height-blocks.idx", std::ios::binary);
    std::vector<std::byte> block_bytes(static_cast<std::size_t>(blocks_size));
    if (!blocks || !read_exact(blocks, block_bytes)) {
        *error = "cannot read height block index";
        return false;
    }
    block_prefix_q8_.clear();
    block_prefix_q8_.reserve(static_cast<std::size_t>(stats_.block_count) + 1U);
    block_prefix_q8_.push_back(0U);
    std::uint64_t prefix = 0;
    for (std::uint64_t index = 0; index < stats_.block_count; ++index) {
        const auto height = read_le<std::uint64_t>(block_bytes, static_cast<std::size_t>(index) * kBlockEntryBytes);
        if (!height || prefix > std::numeric_limits<std::uint64_t>::max() - *height) {
            *error = "invalid height block value";
            return false;
        }
        prefix += *height;
        block_prefix_q8_.push_back(prefix);
    }
    if (prefix != stats_.total_height_q8) {
        *error = "arena total height mismatch";
        return false;
    }
    opened_ = true;
    return true;
}

const ArenaStats& CompactArenaReader::stats() const noexcept {
    return stats_;
}

bool CompactArenaReader::materialize(
    std::uint64_t scroll_y_q8,
    std::uint64_t viewport_height_q8,
    std::uint64_t overscan_q8,
    std::size_t max_records,
    ViewportResult* result,
    std::string* error) const {
    if (!opened_ || result == nullptr || error == nullptr || max_records == 0U || viewport_height_q8 == 0U) {
        if (error != nullptr) {
            *error = "invalid viewport request";
        }
        return false;
    }
    error->clear();
    result->records.clear();
    result->truncated = false;
    result->total_height_q8 = stats_.total_height_q8;

    const std::uint64_t start = scroll_y_q8 > overscan_q8 ? scroll_y_q8 - overscan_q8 : 0U;
    const std::uint64_t visible_end = scroll_y_q8 > std::numeric_limits<std::uint64_t>::max() - viewport_height_q8
                                          ? std::numeric_limits<std::uint64_t>::max()
                                          : scroll_y_q8 + viewport_height_q8;
    const std::uint64_t end = visible_end > std::numeric_limits<std::uint64_t>::max() - overscan_q8
                                  ? std::numeric_limits<std::uint64_t>::max()
                                  : visible_end + overscan_q8;
    result->query_start_q8 = std::min(start, stats_.total_height_q8);
    result->query_end_q8 = std::min(end, stats_.total_height_q8);
    if (result->query_start_q8 >= stats_.total_height_q8) {
        return true;
    }

    const auto upper = std::upper_bound(block_prefix_q8_.begin(), block_prefix_q8_.end(), result->query_start_q8);
    std::size_t block_index = upper == block_prefix_q8_.begin()
                                  ? 0U
                                  : static_cast<std::size_t>(std::distance(block_prefix_q8_.begin(), upper) - 1);
    if (block_index >= static_cast<std::size_t>(stats_.block_count)) {
        block_index = static_cast<std::size_t>(stats_.block_count - 1U);
    }

    std::ifstream heights(root_ / "arena" / "record-heights.idx", std::ios::binary);
    if (!heights) {
        *error = "cannot open record height index";
        return false;
    }

    std::uint64_t current_y = block_prefix_q8_[block_index];
    std::uint64_t record_index = static_cast<std::uint64_t>(block_index) * stats_.config.records_per_block;
    while (record_index < stats_.logical_records && current_y < result->query_end_q8) {
        const std::uint64_t remaining_records = stats_.logical_records - record_index;
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(stats_.config.records_per_block, remaining_records));
        const std::uint64_t height_offset = record_index * kHeightEntryBytes;
        if (!seek_stream(heights, height_offset)) {
            *error = "cannot seek record height block";
            return false;
        }
        std::vector<std::byte> height_bytes(count * kHeightEntryBytes);
        if (!read_exact(heights, height_bytes)) {
            *error = "cannot read record height block";
            return false;
        }
        std::vector<RecordDescriptorLite> descriptors;
        if (!read_record_descriptors(root_ / "records.idx", record_index, count, &descriptors, error)) {
            return false;
        }
        for (std::size_t local = 0; local < count; ++local) {
            const auto height = read_le<std::uint32_t>(height_bytes, local * kHeightEntryBytes);
            if (!height) {
                *error = "invalid record height";
                return false;
            }
            const std::uint64_t record_end = current_y + *height;
