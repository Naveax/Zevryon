            if (record_end > result->query_start_q8 && current_y < result->query_end_q8) {
                if (result->records.size() >= max_records) {
                    result->truncated = true;
                    return true;
                }
                result->records.push_back(MaterializedRecord{
                    record_index + local,
                    descriptors[local].logical_id,
                    current_y,
                    *height,
                    descriptors[local].length,
                });
            }
            current_y = record_end;
            if (current_y >= result->query_end_q8) {
                return true;
            }
        }
        record_index += count;
        ++block_index;
    }
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

} // namespace zevryon::massivedoc
