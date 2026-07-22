    std::uint64_t visible_start_q8,
    std::uint64_t visible_end_q8,
    std::size_t max_fragments,
    std::vector<LayoutFragment>* fragments,
    std::uint64_t* source_bytes_read,
    std::uint64_t* checkpoint_source_offset,
    bool* truncated,
    std::string* error) {
    if (fragments == nullptr || source_bytes_read == nullptr || checkpoint_source_offset == nullptr ||
        truncated == nullptr || error == nullptr || max_fragments == 0U ||
        visible_end_q8 <= visible_start_q8) {
        if (error != nullptr) {
            *error = "invalid checkpoint layout-window request";
        }
        return false;
    }
    error->clear();
    fragments->clear();
    *source_bytes_read = 0U;
    *truncated = false;

    const LayoutCheckpointStats& stats = checkpoint.stats();
    if (stats.record_index != record.record_index || stats.logical_id != record.logical_id ||
        stats.source_bytes != record.source_bytes) {
        *error = "checkpoint does not match materialized record";
        return false;
    }
    const LayoutCheckpointConfig& config = stats.config;
    const LayoutCheckpointEntry& start_entry = checkpoint.entry_for_y(visible_start_q8);
    *checkpoint_source_offset = start_entry.source_offset;

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

    std::uint64_t source_offset = start_entry.source_offset;
    std::uint64_t line_start = source_offset;
    std::uint64_t columns = 0U;
    std::uint64_t line_count = start_entry.line_index;
    std::uint64_t pending_start = 0U;
    std::uint8_t pending_remaining = 0U;
    bool stop = false;

    const auto emit_line = [&](std::uint64_t source_end, bool hard_break) {
        const std::uint64_t local_y =
            saturating_add(top_padding, saturating_multiply(line_count, config.line_height_q8));
        if (intersects(local_y, config.line_height_q8, visible_start_q8, visible_end_q8)) {
            if (fragments->size() < max_fragments) {
                const std::uint64_t raw_width =
                    saturating_multiply(columns, config.average_advance_q8);
                LayoutFragment fragment;
                fragment.record_index = record.record_index;
                fragment.logical_id = record.logical_id;
                fragment.source_start = line_start;
                fragment.source_end = source_end;
                fragment.x_q8 = config.horizontal_padding_q8;
                fragment.y_q8 = local_y;
                fragment.width_q8 = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                    std::max<std::uint64_t>(config.average_advance_q8, raw_width),
                    std::min<std::uint64_t>(
                        content_width, std::numeric_limits<std::uint32_t>::max())));
                fragment.height_q8 = config.line_height_q8;
                fragment.hard_break = hard_break;
                fragments->push_back(fragment);
            } else {
                *truncated = true;
            }
        }
        ++line_count;
        line_start = source_end;
        columns = 0U;
        if (local_y >= visible_end_q8 || fragments->size() >= max_fragments) {
            stop = true;
        }
    };

    const auto consume_codepoint = [&](std::uint64_t end, std::uint32_t value) {
        if (value == 0x0aU) {
            emit_line(end, true);
            return;
        }
        columns = saturating_add(columns, value == 0x09U ? 4U : 1U);
        if (columns >= columns_per_line) {
            emit_line(end, false);
        }
    };

    std::vector<std::byte> chunk;
    while (!stop && source_offset < record.source_bytes) {
        const std::uint64_t remaining = record.source_bytes - source_offset;
        const std::size_t request = static_cast<std::size_t>(
            std::min<std::uint64_t>(kIoWindowBytes, remaining));
        if (!reader.read_record_slice(record.record_index, source_offset, request, &chunk, error)) {
            return false;
        }
        if (chunk.empty()) {
            *error = "checkpoint scan ended before record length";
            return false;
        }
        *source_bytes_read += static_cast<std::uint64_t>(chunk.size());
        for (const std::byte raw : chunk) {
            if (stop) {
                break;
            }
            const std::uint8_t byte =
                static_cast<std::uint8_t>(std::to_integer<unsigned int>(raw));
            bool retry = true;
            while (retry && !stop) {
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
    }
    static_cast<void>(pending_start);
    if (!stop && pending_remaining != 0U) {
        consume_codepoint(source_offset, 0xfffdU);
    }
    if (!stop && (line_start < source_offset || fragments->empty())) {
        emit_line(source_offset, false);
    }
    return true;
}

std::string layout_checkpoint_stats_json(const LayoutCheckpointStats& stats) {
    std::ostringstream output;
    output << "{\"record_index\":" << stats.record_index << ",\"logical_id\":" << stats.logical_id
           << ",\"source_bytes\":" << stats.source_bytes << ",\"measured_height_q8\":"
           << stats.measured_height_q8 << ",\"height_saturated\":"
           << (stats.height_saturated ? "true" : "false") << ",\"entry_count\":" << stats.entry_count
           << ",\"physical_bytes\":" << stats.physical_bytes << ",\"width_q8\":"
           << stats.config.width_q8 << ",\"average_advance_q8\":"
           << stats.config.average_advance_q8 << ",\"line_height_q8\":"
           << stats.config.line_height_q8 << ",\"horizontal_padding_q8\":"
           << stats.config.horizontal_padding_q8 << ",\"vertical_padding_q8\":"
           << stats.config.vertical_padding_q8 << ",\"target_stride_bytes\":"
           << stats.config.target_stride_bytes << '}';
    return output.str();
}

} // namespace zevryon::massivedoc
