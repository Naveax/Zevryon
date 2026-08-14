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
    if (!header) {
        *error = "cannot flush arena header";
        return false;
    }
    header.close();
    if (header.fail()) {
        *error = "cannot close arena header";
        return false;
    }
    const std::uint64_t measured_size = directory_size(arena_root);
    if (measured_size != output.physical_bytes) {
        *error = "arena physical size mismatch";
        return false;
    }
    *stats = output;
    return true;
}

CompactArenaReader::CompactArenaReader(const std::filesystem::path& store_root)
    : root_(store_root), sequence_(256U) {}

std::uint64_t CompactArenaReader::fenwick_prefix(std::size_t block_count) const noexcept {
    std::uint64_t total = 0U;
    while (block_count != 0U) {
        total += fenwick_q8_[block_count];
        block_count -= block_count & (~block_count + 1U);
    }
    return total;
}

void CompactArenaReader::fenwick_replace(
    std::size_t block_index,
    std::uint64_t old_value,
    std::uint64_t new_value) noexcept {
    std::size_t position = block_index + 1U;
    while (position < fenwick_q8_.size()) {
        if (new_value >= old_value) {
            fenwick_q8_[position] += new_value - old_value;
        } else {
            fenwick_q8_[position] -= old_value - new_value;
        }
        position += position & (~position + 1U);
    }
}

bool CompactArenaReader::open(std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    opened_ = false;
    logical_order_generation_ = 0U;
    sequence_ = ChunkedOrderStatisticsSequence{256U};
    block_heights_q8_.clear();
    fenwick_q8_.clear();

    const auto arena_root = root_ / "arena";
    std::ifstream header(arena_root / "arena.meta", std::ios::binary);
    std::array<std::byte, kArenaHeaderBytes> header_bytes{};
    ArenaStats parsed_stats;
    if (!header || !read_exact(header, header_bytes) || !parse_header(header_bytes, &parsed_stats)) {
        *error = "invalid arena header";
        return false;
    }
    stats_ = parsed_stats;

    if (stats_.logical_records >
        std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(kHeightEntryBytes)) {
        *error = "record height index size overflow";
        return false;
    }
    const std::uint64_t expected_block_count =
        stats_.logical_records == 0U
            ? 0U
            : 1U + (stats_.logical_records - 1U) / stats_.config.records_per_block;
    if (stats_.block_count != expected_block_count) {
        *error = "arena block count mismatch";
        return false;
    }
    if (stats_.block_count >
        std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(kBlockEntryBytes)) {
        *error = "height block index size overflow";
        return false;
    }

    std::error_code size_error;
    const std::uint64_t heights_size =
        std::filesystem::file_size(arena_root / "record-heights.idx", size_error);
    const std::uint64_t expected_heights_size =
        stats_.logical_records * static_cast<std::uint64_t>(kHeightEntryBytes);
    if (size_error || heights_size != expected_heights_size) {
        *error = "record height index size mismatch";
        return false;
    }
    const std::uint64_t blocks_size =
        std::filesystem::file_size(arena_root / "height-blocks.idx", size_error);
    const std::uint64_t expected_blocks_size =
        stats_.block_count * static_cast<std::uint64_t>(kBlockEntryBytes);
    if (size_error || blocks_size != expected_blocks_size ||
        stats_.block_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
        blocks_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        *error = "height block index size mismatch";
        return false;
    }

    std::ifstream blocks(arena_root / "height-blocks.idx", std::ios::binary);
    std::vector<std::byte> block_bytes(static_cast<std::size_t>(blocks_size));
    if (!blocks || !read_exact(blocks, block_bytes)) {
        *error = "cannot read height block index";
        return false;
    }
    const std::size_t block_count = static_cast<std::size_t>(stats_.block_count);
    block_heights_q8_.reserve(block_count);
    fenwick_q8_.assign(block_count + 1U, 0U);
    for (std::size_t index = 0U; index < block_count; ++index) {
        const auto height = read_le<std::uint64_t>(block_bytes, index * kBlockEntryBytes);
        if (!height || *height == 0U) {
            *error = "invalid height block value";
            return false;
        }
        block_heights_q8_.push_back(*height);
        std::size_t position = index + 1U;
        while (position < fenwick_q8_.size()) {
            if (fenwick_q8_[position] > std::numeric_limits<std::uint64_t>::max() - *height) {
                *error = "height block prefix overflow";
                return false;
            }
            fenwick_q8_[position] += *height;
            position += position & (~position + 1U);
        }
    }
    if (fenwick_prefix(block_count) != stats_.total_height_q8) {
        *error = "arena total height mismatch";
        return false;
    }

    std::ifstream heights(arena_root / "record-heights.idx", std::ios::binary);
    if (!heights) {
        *error = "cannot open record height index";
        return false;
    }
    std::uint64_t record_index = 0U;
    for (std::size_t block_index = 0U; block_index < block_count; ++block_index) {
        const std::uint64_t remaining = stats_.logical_records - record_index;
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(stats_.config.records_per_block, remaining));
        if (count == 0U) {
            *error = "height block count exceeds logical records";
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

        std::uint64_t measured_block_height = 0U;
        for (std::size_t local = 0U; local < count; ++local) {
            const auto height = read_le<std::uint32_t>(height_bytes, local * kHeightEntryBytes);
            if (!height || *height == 0U) {
                *error = "invalid record height";
                return false;
            }
            if (measured_block_height > std::numeric_limits<std::uint64_t>::max() - *height) {
                *error = "height block aggregate overflow";
                return false;
            }
            measured_block_height += *height;

            const SequenceRecord record{
                descriptors[local].logical_id,
                descriptors[local].length,
                *height,
                0U,
                record_index + static_cast<std::uint64_t>(local),
            };
            std::string sequence_error;
            if (!sequence_.insert(sequence_.stats().aggregate.record_count, record, &sequence_error)) {
                *error = "cannot build physical source sequence: " + sequence_error;
                return false;
            }
        }
        if (measured_block_height != block_heights_q8_[block_index]) {
            *error = "height block aggregate mismatch";
            return false;
        }
        record_index += static_cast<std::uint64_t>(count);
    }

    const auto& physical_sequence_stats = sequence_.stats();
    if (record_index != stats_.logical_records ||
        physical_sequence_stats.aggregate.record_count != stats_.logical_records ||
        physical_sequence_stats.aggregate.layout_height_q8 != stats_.total_height_q8) {
        *error = "physical source sequence aggregate mismatch";
        return false;
    }

    LogicalOrderSnapshot persisted_order;
    if (!load_logical_order_snapshot(
            root_, stats_.logical_records, &persisted_order, error)) {
        return false;
    }
    const auto physical_snapshot = sequence_.snapshot();
    ChunkedOrderStatisticsSequence restored{256U};
    for (std::uint64_t logical_index = 0U;
         logical_index < stats_.logical_records;
         ++logical_index) {
        const std::uint64_t source_record_index =
            persisted_order.source_record_indices[static_cast<std::size_t>(logical_index)];
        SequencePosition physical_position;
        std::string sequence_error;
        if (!physical_snapshot.at(
                source_record_index, &physical_position, &sequence_error)) {
            *error = "cannot resolve persisted physical source record: " + sequence_error;
            return false;
        }
        if (physical_position.record.source_record_index != source_record_index) {
            *error = "physical source sequence identity mismatch";
            return false;
        }
        if (!restored.insert(
                restored.stats().aggregate.record_count,
                physical_position.record,
                &sequence_error)) {
            *error = "cannot restore persisted logical order: " + sequence_error;
            return false;
        }
    }
    if (restored.stats().aggregate.record_count != stats_.logical_records ||
        restored.stats().aggregate.layout_height_q8 != stats_.total_height_q8) {
        *error = "restored logical order aggregate mismatch";
        return false;
    }
    sequence_ = std::move(restored);
    logical_order_generation_ = persisted_order.generation;

    opened_ = true;
    return true;
}

const ArenaStats& CompactArenaReader::stats() const noexcept {
    return stats_;
}

ChunkedOrderStatisticsSequence::Snapshot CompactArenaReader::logical_snapshot() const noexcept {
    return sequence_.snapshot();
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

    const auto snapshot = sequence_.snapshot();
    const std::uint64_t total_height_q8 = snapshot.stats().aggregate.layout_height_q8;
    result->total_height_q8 = total_height_q8;

    const std::uint64_t start = scroll_y_q8 > overscan_q8 ? scroll_y_q8 - overscan_q8 : 0U;
    const std::uint64_t visible_end =
        scroll_y_q8 > std::numeric_limits<std::uint64_t>::max() - viewport_height_q8
            ? std::numeric_limits<std::uint64_t>::max()
            : scroll_y_q8 + viewport_height_q8;
    const std::uint64_t end =
        visible_end > std::numeric_limits<std::uint64_t>::max() - overscan_q8
            ? std::numeric_limits<std::uint64_t>::max()
            : visible_end + overscan_q8;
    result->query_start_q8 = std::min(start, total_height_q8);
    result->query_end_q8 = std::min(end, total_height_q8);
    if (snapshot.empty() || result->query_start_q8 >= total_height_q8) {
        return true;
    }

    SequencePosition position;
    if (!snapshot.locate_height_offset(result->query_start_q8, &position, error)) {
        return false;
    }
    std::uint64_t record_index = position.record_index;
    while (record_index < snapshot.stats().aggregate.record_count &&
           position.y_q8 < result->query_end_q8) {
        if (record_index != position.record_index &&
            !snapshot.at(record_index, &position, error)) {
            return false;
        }
        if (position.y_q8 >
            std::numeric_limits<std::uint64_t>::max() - position.record.height_q8) {
            *error = "logical sequence record height overflow";
            return false;
        }
        const std::uint64_t record_end =
            position.y_q8 + static_cast<std::uint64_t>(position.record.height_q8);
        if (record_end > result->query_start_q8 && position.y_q8 < result->query_end_q8) {
            if (result->records.size() >= max_records) {
                result->truncated = true;
                return true;
            }
            result->records.push_back(MaterializedRecord{
                position.record_index,
                position.record.logical_id,
                position.y_q8,
                position.record.height_q8,
                position.record.text_bytes,
                position.record.source_record_index,
            });
        }
        if (record_end >= result->query_end_q8) {
            return true;
        }
        ++record_index;
    }
    return true;
}
