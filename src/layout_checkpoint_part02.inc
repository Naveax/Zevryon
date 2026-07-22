    }
    if (source_offset != info.source_bytes) {
        *error = "checkpoint scan length differs from record descriptor";
        return false;
    }
    if (pending_remaining != 0U) {
        consume_codepoint(source_offset, 0xfffdU);
    }
    if (line_start < source_offset || !emitted_any || columns != 0U) {
        emit_line(source_offset);
    }

    LayoutCheckpointStats output;
    output.record_index = record_index;
    output.logical_id = info.logical_id;
    output.source_bytes = info.source_bytes;
    output.config = config;
    const std::uint64_t measured = saturating_add(
        saturating_multiply(line_count, config.line_height_q8), config.vertical_padding_q8);
    output.height_saturated = measured > std::numeric_limits<std::uint32_t>::max();
    output.measured_height_q8 = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(measured, std::numeric_limits<std::uint32_t>::max()));
    output.entry_count = static_cast<std::uint64_t>(entries.size());
    if (entries.size() >
        (std::numeric_limits<std::uint64_t>::max() - kCheckpointHeaderBytes) / kCheckpointEntryBytes) {
        *error = "checkpoint index size overflow";
        return false;
    }
    output.physical_bytes =
        kCheckpointHeaderBytes + static_cast<std::uint64_t>(entries.size()) * kCheckpointEntryBytes;

    const std::vector<std::byte> entry_bytes = serialize_entries(entries);
    const std::uint64_t checksum = fnv1a64(entry_bytes);
    const std::vector<std::byte> header_bytes = serialize_header(output, checksum);
    if (header_bytes.size() != kCheckpointHeaderBytes ||
        entry_bytes.size() != entries.size() * kCheckpointEntryBytes) {
        *error = "internal checkpoint serialization mismatch";
        return false;
    }

    const auto final_path = layout_checkpoint_path(store_root, record_index, config);
    std::error_code fs_error;
    std::filesystem::create_directories(final_path.parent_path(), fs_error);
    if (fs_error) {
        *error = "cannot create checkpoint directory";
        return false;
    }
    auto temporary_path = final_path;
    temporary_path += ".tmp";
    std::filesystem::remove(temporary_path, fs_error);
    fs_error.clear();
    std::ofstream stream(temporary_path, std::ios::binary | std::ios::trunc);
    if (!stream || !write_all(stream, header_bytes) || !write_all(stream, entry_bytes)) {
        *error = "cannot write layout checkpoint";
        return false;
    }
    stream.flush();
    if (!stream) {
        *error = "cannot flush layout checkpoint";
        return false;
    }
    stream.close();
    if (stream.fail()) {
        *error = "cannot close layout checkpoint";
        return false;
    }
    const std::uint64_t written_size = std::filesystem::file_size(temporary_path, fs_error);
    if (fs_error || written_size != output.physical_bytes) {
        *error = "layout checkpoint physical size mismatch";
        return false;
    }
    std::filesystem::remove(final_path, fs_error);
    fs_error.clear();
    std::filesystem::rename(temporary_path, final_path, fs_error);
    if (fs_error) {
        *error = "cannot publish layout checkpoint";
        return false;
    }
    *stats = output;
    return true;
}

bool LayoutCheckpointIndex::open(
    const std::filesystem::path& store_root,
    std::uint64_t record_index,
    std::uint64_t logical_id,
    std::uint64_t source_bytes,
    LayoutCheckpointConfig config,
    std::string* error) {
    if (error == nullptr || !valid_config(config)) {
        if (error != nullptr) {
            *error = "invalid layout-checkpoint open request";
        }
        return false;
    }
    error->clear();
    const auto path = layout_checkpoint_path(store_root, record_index, config);
    std::ifstream stream(path, std::ios::binary);
    std::array<std::byte, kCheckpointHeaderBytes> header_bytes{};
    std::uint64_t checksum = 0U;
    if (!stream || !read_exact(stream, header_bytes) || !parse_header(header_bytes, &stats_, &checksum)) {
        *error = "invalid layout checkpoint header";
        return false;
    }
    if (stats_.record_index != record_index || stats_.logical_id != logical_id ||
        stats_.source_bytes != source_bytes || !configs_equal(stats_.config, config)) {
        *error = "layout checkpoint identity/config mismatch";
        return false;
    }
    if (stats_.entry_count == 0U ||
        stats_.entry_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
        stats_.entry_count >
            (std::numeric_limits<std::uint64_t>::max() - kCheckpointHeaderBytes) /
                kCheckpointEntryBytes ||
        stats_.physical_bytes !=
            kCheckpointHeaderBytes + stats_.entry_count * kCheckpointEntryBytes) {
        *error = "layout checkpoint size metadata mismatch";
        return false;
    }

    std::error_code size_error;
    const std::uint64_t actual_size = std::filesystem::file_size(path, size_error);
    if (size_error || actual_size != stats_.physical_bytes) {
        *error = "layout checkpoint file size mismatch";
        return false;
    }
    const std::size_t entry_count = static_cast<std::size_t>(stats_.entry_count);
    std::vector<std::byte> entry_bytes(entry_count * kCheckpointEntryBytes);
    if (!read_exact(stream, entry_bytes) || fnv1a64(entry_bytes) != checksum) {
        *error = "layout checkpoint checksum mismatch";
        return false;
    }

    entries_.clear();
    entries_.reserve(entry_count);
    for (std::size_t index = 0U; index < entry_count; ++index) {
        const std::size_t offset = index * kCheckpointEntryBytes;
        const auto source_offset = read_le<std::uint64_t>(entry_bytes, offset);
        const auto line_index = read_le<std::uint64_t>(entry_bytes, offset + 8U);
        const auto y_q8 = read_le<std::uint64_t>(entry_bytes, offset + 16U);
        if (!source_offset || !line_index || !y_q8 || *source_offset > source_bytes) {
            *error = "invalid layout checkpoint entry";
            return false;
        }
        if (!entries_.empty()) {
            const LayoutCheckpointEntry& previous = entries_.back();
            if (*source_offset <= previous.source_offset || *line_index <= previous.line_index ||
                *y_q8 <= previous.y_q8) {
                *error = "non-monotonic layout checkpoint entry";
                return false;
            }
        }
        entries_.push_back(LayoutCheckpointEntry{*source_offset, *line_index, *y_q8});
    }
    const std::uint64_t expected_first_y = config.vertical_padding_q8 / 2U;
    if (entries_.front().source_offset != 0U || entries_.front().line_index != 0U ||
        entries_.front().y_q8 != expected_first_y) {
        *error = "layout checkpoint missing canonical first entry";
        return false;
    }
    opened_ = true;
    return true;
}

const LayoutCheckpointStats& LayoutCheckpointIndex::stats() const noexcept {
    return stats_;
}

const std::vector<LayoutCheckpointEntry>& LayoutCheckpointIndex::entries() const noexcept {
    return entries_;
}

const LayoutCheckpointEntry& LayoutCheckpointIndex::entry_for_y(std::uint64_t local_y_q8) const noexcept {
    const auto found = std::upper_bound(
        entries_.begin(),
        entries_.end(),
        local_y_q8,
        [](std::uint64_t value, const LayoutCheckpointEntry& entry) { return value < entry.y_q8; });
    return found == entries_.begin() ? entries_.front() : *std::prev(found);
}

bool scan_layout_window_from_checkpoint(
    const std::filesystem::path& store_root,
    const MaterializedRecord& record,
    const LayoutCheckpointIndex& checkpoint,
