bool CompactArenaReader::move_logical_record(
    std::uint64_t from_index,
    std::uint64_t to_index,
    std::string* error) {
    if (!opened_ || error == nullptr || from_index >= stats_.logical_records ||
        to_index >= stats_.logical_records) {
        if (error != nullptr) {
            *error = "invalid logical move";
        }
        return false;
    }
    error->clear();
    if (from_index == to_index) {
        return true;
    }
    if (logical_order_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        *error = "logical order generation exhausted";
        return false;
    }
    std::lock_guard<std::mutex> persistence_lock(compact_arena_persistence_mutex());

    auto candidate = sequence_.fork_shared_root();
    std::string sequence_error;
    if (!candidate.move(from_index, to_index, &sequence_error)) {
        *error = "cannot build logical move candidate: " + sequence_error;
        return false;
    }

    const auto candidate_snapshot = candidate.snapshot();
    std::vector<std::uint64_t> source_record_indices;
    source_record_indices.reserve(static_cast<std::size_t>(stats_.logical_records));
    for (std::uint64_t logical_index = 0U;
         logical_index < stats_.logical_records;
         ++logical_index) {
        SequencePosition position;
        if (!candidate_snapshot.at(logical_index, &position, &sequence_error)) {
            *error = "cannot serialize logical move candidate: " + sequence_error;
            return false;
        }
        source_record_indices.push_back(position.record.source_record_index);
    }

    const std::uint64_t next_generation = logical_order_generation_ + 1U;
    if (!publish_logical_order_snapshot(
            root_, next_generation, source_record_indices, error)) {
        opened_ = false;
        return false;
    }

    sequence_ = std::move(candidate);
    logical_order_generation_ = next_generation;
    return true;
}

bool CompactArenaReader::update_height(
    std::uint64_t record_index,
    std::uint32_t new_height_q8,
    HeightUpdateResult* result,
    std::string* error) {
    if (!opened_ || result == nullptr || error == nullptr || new_height_q8 == 0U ||
        record_index >= stats_.logical_records) {
        if (error != nullptr) {
            *error = "invalid height update";
        }
        return false;
    }
    error->clear();
    std::lock_guard<std::mutex> persistence_lock(compact_arena_persistence_mutex());

    SequencePosition sequence_position;
    std::string sequence_error;
    if (!sequence_.at(record_index, &sequence_position, &sequence_error)) {
        *error = "cannot resolve logical sequence height: " + sequence_error;
        return false;
    }
    const std::uint64_t source_record_index = sequence_position.record.source_record_index;
    if (source_record_index >= stats_.logical_records) {
        *error = "logical sequence source record is outside persisted arena";
        return false;
    }
    const std::size_t block_index = static_cast<std::size_t>(
        source_record_index / stats_.config.records_per_block);
    if (block_index >= block_heights_q8_.size()) {
        *error = "height update block is outside arena";
        return false;
    }

    const auto arena_root = root_ / "arena";
    const std::uint64_t height_offset = source_record_index * kHeightEntryBytes;
    std::fstream heights(arena_root / "record-heights.idx", std::ios::binary | std::ios::in | std::ios::out);
    std::array<std::byte, kHeightEntryBytes> old_height_bytes{};
    if (!heights || !seek_stream(heights, height_offset) || !read_exact(heights, old_height_bytes)) {
        *error = "cannot read existing record height";
        return false;
    }
    const auto old_height = read_le<std::uint32_t>(old_height_bytes, 0U);
    if (!old_height || *old_height == 0U) {
        *error = "invalid existing record height";
        return false;
    }
    if (sequence_position.record.height_q8 != *old_height) {
        *error = "logical sequence height differs from persisted source record";
        return false;
    }

    const std::uint64_t old_block_height = block_heights_q8_[block_index];
    if (old_block_height < *old_height) {
        *error = "height block underflow";
        return false;
    }
    const std::uint64_t block_without_record = old_block_height - *old_height;
    if (block_without_record > std::numeric_limits<std::uint64_t>::max() - new_height_q8) {
        *error = "height block overflow";
        return false;
    }
    const std::uint64_t new_block_height = block_without_record + new_height_q8;

    std::uint64_t new_total_height = stats_.total_height_q8;
    if (new_height_q8 >= *old_height) {
        const std::uint64_t delta = static_cast<std::uint64_t>(new_height_q8 - *old_height);
        if (new_total_height > std::numeric_limits<std::uint64_t>::max() - delta) {
            *error = "arena total height overflow";
            return false;
        }
        new_total_height += delta;
    } else {
        const std::uint64_t delta = static_cast<std::uint64_t>(*old_height - new_height_q8);
        if (new_total_height < delta) {
            *error = "arena total height underflow";
            return false;
        }
        new_total_height -= delta;
    }

    const auto write_height_value = [&](std::uint32_t value) {
        std::vector<std::byte> encoded;
        encoded.reserve(kHeightEntryBytes);
        append_le(encoded, value);
        heights.clear();
        heights.seekp(static_cast<std::streamoff>(height_offset), std::ios::beg);
        return static_cast<bool>(heights) && write_all(heights, encoded) && static_cast<bool>(heights.flush());
    };

    std::fstream blocks(arena_root / "height-blocks.idx", std::ios::binary | std::ios::in | std::ios::out);
    const std::uint64_t block_offset = static_cast<std::uint64_t>(block_index) * kBlockEntryBytes;
    const auto write_block_value = [&](std::uint64_t value) {
        std::vector<std::byte> encoded;
        encoded.reserve(kBlockEntryBytes);
        append_le(encoded, value);
        blocks.clear();
        blocks.seekp(static_cast<std::streamoff>(block_offset), std::ios::beg);
        return static_cast<bool>(blocks) && write_all(blocks, encoded) && static_cast<bool>(blocks.flush());
    };

    const auto write_header_value = [&](std::uint64_t total_height) {
        ArenaStats persisted = stats_;
        persisted.total_height_q8 = total_height;
        const auto bytes = serialize_header(persisted);
        std::ofstream header(arena_root / "arena.meta", std::ios::binary | std::ios::trunc);
        return header && write_all(header, bytes) && static_cast<bool>(header.flush());
    };

    if (!blocks || !write_height_value(new_height_q8)) {
        *error = "cannot persist source-record height";
        return false;
    }
    if (!write_block_value(new_block_height)) {
        static_cast<void>(write_height_value(*old_height));
        *error = "cannot persist height block";
        return false;
    }
    if (!write_header_value(new_total_height)) {
        static_cast<void>(write_block_value(old_block_height));
        static_cast<void>(write_height_value(*old_height));
        *error = "cannot persist arena header";
        return false;
    }

    std::uint32_t sequence_old_height = 0U;
    if (!sequence_.update_height(
            record_index,
            new_height_q8,
            &sequence_old_height,
            &sequence_error)) {
        const bool header_rolled_back = write_header_value(stats_.total_height_q8);
        const bool block_rolled_back = write_block_value(old_block_height);
        const bool height_rolled_back = write_height_value(*old_height);
        *error = "cannot publish logical sequence height update: " + sequence_error;
        if (!header_rolled_back || !block_rolled_back || !height_rolled_back) {
            *error += "; persistence rollback failed";
        }
        return false;
    }
    static_cast<void>(sequence_old_height);

    fenwick_replace(block_index, old_block_height, new_block_height);
    block_heights_q8_[block_index] = new_block_height;
    stats_.total_height_q8 = new_total_height;
    *result = HeightUpdateResult{
        record_index,
        static_cast<std::uint64_t>(block_index),
        *old_height,
        new_height_q8,
        new_total_height,
    };
    return true;
}

std::string arena_stats_json(const ArenaStats& stats) {
    std::ostringstream output;
    output << "{\"logical_records\":" << stats.logical_records << ",\"logical_nodes\":" << stats.logical_nodes
           << ",\"total_height_q8\":" << stats.total_height_q8 << ",\"block_count\":" << stats.block_count
           << ",\"physical_bytes\":" << stats.physical_bytes << ",\"records_per_block\":"
           << stats.config.records_per_block << ",\"estimated_bytes_per_line\":"
           << stats.config.estimated_bytes_per_line << ",\"line_height_q8\":" << stats.config.line_height_q8
           << ",\"vertical_padding_q8\":" << stats.config.vertical_padding_q8 << '}';
    return output.str();
}

std::string viewport_json(const ViewportResult& result) {
    std::ostringstream output;
    output << "{\"query_start_q8\":" << result.query_start_q8 << ",\"query_end_q8\":" << result.query_end_q8
           << ",\"total_height_q8\":" << result.total_height_q8 << ",\"truncated\":"
           << (result.truncated ? "true" : "false") << ",\"count\":" << result.records.size() << ",\"records\":[";
    bool first = true;
    for (const auto& record : result.records) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << "{\"record_index\":" << record.record_index << ",\"logical_id\":" << record.logical_id
               << ",\"y_q8\":" << record.y_q8 << ",\"height_q8\":" << record.height_q8
               << ",\"source_bytes\":" << record.source_bytes << '}';
    }
    output << "]}";
    return output.str();
}

std::string height_update_json(const HeightUpdateResult& result) {
    std::ostringstream output;
    output << "{\"record_index\":" << result.record_index << ",\"block_index\":" << result.block_index
           << ",\"old_height_q8\":" << result.old_height_q8 << ",\"new_height_q8\":"
           << result.new_height_q8 << ",\"total_height_q8\":" << result.total_height_q8 << '}';
    return output.str();
}

} // namespace zevryon::massivedoc
