std::vector<SearchHit> StoreReader::find_unicode_bounded(
    std::string_view query_utf8,
    std::size_t max_hits,
    const SearchCancel& cancel,
    const UnicodeSearchOptions& options,
    UnicodeSearchExecutionStats* execution,
    std::string* error) const {
    UnicodeSearchExecutionStats local_execution;
    UnicodeSearchExecutionStats* const active_execution =
        execution != nullptr ? execution : &local_execution;
    *active_execution = {};
    error->clear();

    std::vector<SearchHit> hits;
    if (!impl_->opened) {
        *error = "store is not open";
        return hits;
    }
    if (query_utf8.empty() || max_hits == 0U) {
        return hits;
    }

    detail::UnicodeSearchRuntime runtime(detail::UnicodeSearchRuntimeLimits{
        options.max_query_bytes,
        options.max_query_codepoints,
        options.max_pending_codepoints});
    if (!runtime.build_pattern(query_utf8, error)) {
        return {};
    }
    active_execution->query_normalized_codepoints =
        static_cast<std::uint64_t>(runtime.pattern_codepoints());

    constexpr std::size_t kCancellationBatchBytes = 4096U;
    for (std::uint64_t record_index = 0U;
         record_index < impl_->stats.corpus.logical_records && hits.size() < max_hits;
         ++record_index) {
        if (cancel && cancel()) {
            active_execution->cancelled = true;
            *error = "search cancelled";
            return {};
        }

        RecordDescriptor record;
        if (!impl_->read_record_descriptor(record_index, &record, error)) {
            return {};
        }

        runtime.reset_record();
        ++active_execution->records_scanned;
        bool pipeline_ok = true;
        bool cancelled_during_record = false;
        std::uint64_t record_offset = 0U;
        std::string runtime_error;

        const bool read_ok = read_record(
            record_index,
            [&](std::span<const std::byte> bytes) {
                std::size_t position = 0U;
                while (position < bytes.size()) {
                    if (cancel && cancel()) {
                        cancelled_during_record = true;
                        return false;
                    }
                    const std::size_t amount = std::min(
                        kCancellationBatchBytes,
                        bytes.size() - position);
                    if (!runtime.feed_record(
                            bytes.subspan(position, amount),
                            record_offset,
                            &runtime_error)) {
                        pipeline_ok = false;
                        return false;
                    }
                    record_offset += static_cast<std::uint64_t>(amount);
                    position += amount;
                    if (runtime.found()) {
                        return false;
                    }
                }
                return true;
            },
            error);

        if (!read_ok) {
            const auto runtime_stats = runtime.stats();
            active_execution->source_bytes_decoded += runtime_stats.source_bytes_decoded;
            active_execution->normalized_codepoints += runtime_stats.normalized_codepoints;
            return {};
        }
        if (cancelled_during_record) {
            const auto runtime_stats = runtime.stats();
            active_execution->source_bytes_decoded += runtime_stats.source_bytes_decoded;
            active_execution->normalized_codepoints += runtime_stats.normalized_codepoints;
            active_execution->cancelled = true;
            *error = "search cancelled";
            return {};
        }
        if (!pipeline_ok) {
            const auto runtime_stats = runtime.stats();
            active_execution->source_bytes_decoded += runtime_stats.source_bytes_decoded;
            active_execution->normalized_codepoints += runtime_stats.normalized_codepoints;
            *error = "Unicode search record pipeline failed: " + runtime_error;
            return {};
        }
        if (!runtime.found() && !runtime.finish_record(&runtime_error)) {
            const auto runtime_stats = runtime.stats();
            active_execution->source_bytes_decoded += runtime_stats.source_bytes_decoded;
            active_execution->normalized_codepoints += runtime_stats.normalized_codepoints;
            *error = "Unicode search record finalization failed: " + runtime_error;
            return {};
        }

        const auto runtime_stats = runtime.stats();
        active_execution->source_bytes_decoded += runtime_stats.source_bytes_decoded;
        active_execution->normalized_codepoints += runtime_stats.normalized_codepoints;
        if (runtime.found()) {
            hits.push_back(SearchHit{
                record_index,
                record.logical_id,
                runtime.match_source_start()});
        }
    }

    return hits;
}

