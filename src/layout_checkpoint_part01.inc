    return bytes;
}

bool parse_header(std::span<const std::byte> bytes, LayoutCheckpointStats* stats, std::uint64_t* checksum) {
    if (bytes.size() != kCheckpointHeaderBytes || stats == nullptr || checksum == nullptr) {
        return false;
    }
    for (std::size_t index = 0U; index < kCheckpointMagic.size(); ++index) {
        if (std::to_integer<unsigned char>(bytes[index]) !=
            static_cast<unsigned char>(kCheckpointMagic[index])) {
            return false;
        }
    }
    const auto version = read_le<std::uint32_t>(bytes, 8U);
    const auto flags = read_le<std::uint32_t>(bytes, 12U);
    const auto record_index = read_le<std::uint64_t>(bytes, 16U);
    const auto logical_id = read_le<std::uint64_t>(bytes, 24U);
    const auto source_bytes = read_le<std::uint64_t>(bytes, 32U);
    const auto width_q8 = read_le<std::uint32_t>(bytes, 40U);
    const auto average_advance_q8 = read_le<std::uint32_t>(bytes, 44U);
    const auto line_height_q8 = read_le<std::uint32_t>(bytes, 48U);
    const auto horizontal_padding_q8 = read_le<std::uint32_t>(bytes, 52U);
    const auto vertical_padding_q8 = read_le<std::uint32_t>(bytes, 56U);
    const auto stride_bytes = read_le<std::uint32_t>(bytes, 60U);
    const auto measured_height_q8 = read_le<std::uint32_t>(bytes, 64U);
    const auto entry_count = read_le<std::uint64_t>(bytes, 72U);
    const auto physical_bytes = read_le<std::uint64_t>(bytes, 80U);
    const auto parsed_checksum = read_le<std::uint64_t>(bytes, 88U);
    if (!version || *version != kCheckpointVersion || !flags || !record_index || !logical_id || !source_bytes ||
        !width_q8 || !average_advance_q8 || !line_height_q8 || !horizontal_padding_q8 ||
        !vertical_padding_q8 || !stride_bytes || !measured_height_q8 || !entry_count || !physical_bytes ||
        !parsed_checksum) {
        return false;
    }
    stats->record_index = *record_index;
    stats->logical_id = *logical_id;
    stats->source_bytes = *source_bytes;
    stats->config.width_q8 = *width_q8;
    stats->config.average_advance_q8 = *average_advance_q8;
    stats->config.line_height_q8 = *line_height_q8;
    stats->config.horizontal_padding_q8 = *horizontal_padding_q8;
    stats->config.vertical_padding_q8 = *vertical_padding_q8;
    stats->config.target_stride_bytes = *stride_bytes;
    stats->measured_height_q8 = *measured_height_q8;
    stats->height_saturated = (*flags & kCheckpointFlagHeightSaturated) != 0U;
    stats->entry_count = *entry_count;
    stats->physical_bytes = *physical_bytes;
    *checksum = *parsed_checksum;
    return valid_config(stats->config);
}

bool intersects(
    std::uint64_t start,
    std::uint64_t height,
    std::uint64_t query_start,
    std::uint64_t query_end) noexcept {
    const std::uint64_t end = saturating_add(start, height);
    return end > query_start && start < query_end;
}

} // namespace

std::filesystem::path layout_checkpoint_path(
    const std::filesystem::path& store_root,
    std::uint64_t record_index,
    const LayoutCheckpointConfig& config) {
    std::ostringstream filename;
    filename << "record-" << std::setw(20) << std::setfill('0') << record_index << '-' << std::hex
             << std::setw(16) << std::setfill('0') << config_hash(config) << ".zcp";
    return store_root / "arena" / "layout-checkpoints" / filename.str();
}

bool build_layout_checkpoint(
    const std::filesystem::path& store_root,
    std::uint64_t record_index,
    LayoutCheckpointConfig config,
    LayoutCheckpointStats* stats,
    std::string* error) {
    if (stats == nullptr || error == nullptr || !valid_config(config)) {
        if (error != nullptr) {
            *error = "invalid layout-checkpoint build request";
        }
        return false;
    }
    error->clear();

    RecordInfo info;
    if (!read_record_info(store_root, record_index, &info, error)) {
        return false;
    }
    StoreReader reader(store_root);
    if (!reader.open(error)) {
        return false;
    }

    const std::uint64_t horizontal = static_cast<std::uint64_t>(config.horizontal_padding_q8) * 2U;
    const std::uint64_t content_width = config.width_q8 > horizontal
                                            ? static_cast<std::uint64_t>(config.width_q8) - horizontal
                                            : static_cast<std::uint64_t>(config.average_advance_q8);
    const std::uint64_t columns_per_line = std::max<std::uint64_t>(
        1U, content_width / std::max<std::uint64_t>(1U, config.average_advance_q8));
    const std::uint64_t top_padding = config.vertical_padding_q8 / 2U;

    std::vector<LayoutCheckpointEntry> entries;
    entries.push_back(LayoutCheckpointEntry{0U, 0U, top_padding});
    std::uint64_t source_offset = 0U;
    std::uint64_t line_start = 0U;
    std::uint64_t columns = 0U;
    std::uint64_t line_count = 0U;
    std::uint64_t pending_start = 0U;
    std::uint8_t pending_remaining = 0U;
    std::uint64_t next_checkpoint = config.target_stride_bytes;
    bool emitted_any = false;

    const auto advance_checkpoint_target = [&](std::uint64_t source_end) {
        while (next_checkpoint <= source_end) {
            if (next_checkpoint >
                std::numeric_limits<std::uint64_t>::max() - config.target_stride_bytes) {
                next_checkpoint = std::numeric_limits<std::uint64_t>::max();
                return;
            }
            next_checkpoint += config.target_stride_bytes;
        }
    };

    const auto emit_line = [&](std::uint64_t source_end) {
        ++line_count;
        line_start = source_end;
        columns = 0U;
        emitted_any = true;
        if (source_end >= next_checkpoint && source_end < info.source_bytes) {
            const std::uint64_t y_q8 =
                saturating_add(top_padding, saturating_multiply(line_count, config.line_height_q8));
            entries.push_back(LayoutCheckpointEntry{source_end, line_count, y_q8});
            advance_checkpoint_target(source_end);
        }
    };

    const auto consume_codepoint = [&](std::uint64_t end, std::uint32_t value) {
        if (value == 0x0aU) {
            emit_line(end);
            return;
        }
        columns = saturating_add(columns, value == 0x09U ? 4U : 1U);
        if (columns >= columns_per_line) {
            emit_line(end);
        }
    };

    const bool read_ok = reader.read_record(
        record_index,
        [&](std::span<const std::byte> bytes) {
            for (const std::byte raw : bytes) {
                const std::uint8_t byte =
                    static_cast<std::uint8_t>(std::to_integer<unsigned int>(raw));
                bool retry = true;
                while (retry) {
                    retry = false;
                    if (pending_remaining != 0U) {
                        if ((byte & 0xc0U) == 0x80U) {
                            --pending_remaining;
                            ++source_offset;
                            if (pending_remaining == 0U) {
                                consume_codepoint(source_offset, 0xfffdU);
                            }
                            continue;
                        }
                        consume_codepoint(source_offset, 0xfffdU);
                        pending_remaining = 0U;
                        retry = true;
                        continue;
                    }
                    ++source_offset;
                    if (byte < 0x80U) {
                        consume_codepoint(source_offset, byte);
                    } else if ((byte & 0xe0U) == 0xc0U) {
                        pending_start = source_offset - 1U;
                        pending_remaining = 1U;
                    } else if ((byte & 0xf0U) == 0xe0U) {
                        pending_start = source_offset - 1U;
                        pending_remaining = 2U;
                    } else if ((byte & 0xf8U) == 0xf0U) {
                        pending_start = source_offset - 1U;
                        pending_remaining = 3U;
                    } else {
                        consume_codepoint(source_offset, 0xfffdU);
                    }
                }
            }
            return true;
        },
        error);
    static_cast<void>(pending_start);
    if (!read_ok) {
        return false;
