            impl_->stats.corpus.logical_records,
            first_record + impl_->config.records_per_search_block);
        const std::size_t record_count = static_cast<std::size_t>(end_record - first_record);
        std::vector<std::byte> record_bytes(record_count * kRecordDescriptorBytes);
        if (!seek_stream(record_index, first_record * kRecordDescriptorBytes) || !read_exact(record_index, record_bytes)) {
            *error = "cannot read candidate record block";
            return {};
        }
        std::vector<RecordDescriptor> records;
        records.reserve(record_count);
        for (std::size_t index = 0U; index < record_count; ++index) {
            const auto parsed = parse_record(std::span<const std::byte>(
                record_bytes.data() + index * kRecordDescriptorBytes, kRecordDescriptorBytes));
            if (!parsed) {
                *error = "invalid record descriptor in candidate block";
                return {};
            }
            records.push_back(*parsed);
        }
        if (records.empty()) {
            continue;
        }
        const std::uint64_t first_chunk = records.front().first_chunk;
        const RecordDescriptor& last_record = records.back();
        const std::uint64_t end_chunk = last_record.first_chunk + last_record.chunk_count;
        if (end_chunk < first_chunk || end_chunk > impl_->stats.chunk_count) {
            *error = "candidate chunk range is invalid";
            return {};
        }
        const std::size_t chunk_count = static_cast<std::size_t>(end_chunk - first_chunk);
        std::vector<std::byte> chunk_bytes(chunk_count * kChunkDescriptorBytes);
        if (!seek_stream(chunk_index, first_chunk * kChunkDescriptorBytes) || !read_exact(chunk_index, chunk_bytes)) {
            *error = "cannot read candidate chunk block";
            return {};
        }
        std::vector<ChunkDescriptor> chunks;
        chunks.reserve(chunk_count);
        for (std::size_t index = 0U; index < chunk_count; ++index) {
            const auto parsed = parse_chunk(std::span<const std::byte>(
                chunk_bytes.data() + index * kChunkDescriptorBytes, kChunkDescriptorBytes));
            if (!parsed) {
                *error = "invalid chunk descriptor in candidate block";
                return {};
            }
            chunks.push_back(*parsed);
        }

        for (std::size_t relative_record = 0U; relative_record < records.size() && hits.size() < max_hits; ++relative_record) {
            const RecordDescriptor& record = records[relative_record];
            std::vector<std::byte> carry;
            carry.reserve(query.size() > 1U ? query.size() - 1U : 0U);
            std::uint64_t processed = 0U;
            bool found = false;
            std::uint64_t found_offset = 0U;
            const std::uint64_t local_first_chunk = record.first_chunk - first_chunk;
            for (std::uint32_t relative_chunk = 0U; relative_chunk < record.chunk_count && !found; ++relative_chunk) {
                const std::uint64_t local_index = local_first_chunk + relative_chunk;
                if (local_index >= chunks.size()) {
                    *error = "record references a chunk outside candidate block";
                    return {};
                }
                const ChunkDescriptor& chunk = chunks[static_cast<std::size_t>(local_index)];
                if (open_segment != chunk.segment_id) {
                    segment_stream.close();
                    segment_stream.open(segment_path(impl_->root, chunk.segment_id), std::ios::binary);
                    open_segment = chunk.segment_id;
                }
                if (!segment_stream || !seek_stream(segment_stream, chunk.offset)) {
                    *error = "cannot open or seek candidate segment";
                    return {};
                }
                std::uint64_t remaining = chunk.length;
                while (remaining != 0U && !found) {
                    const std::size_t amount = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, io_buffer.size()));
                    if (!read_exact(segment_stream, std::span<std::byte>(io_buffer.data(), amount))) {
                        *error = "truncated candidate segment";
                        return {};
                    }
                    std::vector<std::byte> window;
                    window.reserve(carry.size() + amount);
                    window.insert(window.end(), carry.begin(), carry.end());
                    window.insert(window.end(), io_buffer.begin(), io_buffer.begin() + static_cast<std::ptrdiff_t>(amount));
                    const auto position = std::search(window.begin(), window.end(), query_bytes.begin(), query_bytes.end());
                    if (position != window.end()) {
                        const std::size_t local = static_cast<std::size_t>(std::distance(window.begin(), position));
                        found_offset = processed - static_cast<std::uint64_t>(carry.size()) + static_cast<std::uint64_t>(local);
                        found = true;
                        break;
                    }
                    processed += static_cast<std::uint64_t>(amount);
                    remaining -= static_cast<std::uint64_t>(amount);
                    const std::size_t keep = std::min<std::size_t>(query.size() > 1U ? query.size() - 1U : 0U, window.size());
                    carry.assign(window.end() - static_cast<std::ptrdiff_t>(keep), window.end());
                }
            }
            if (found) {
                hits.push_back(SearchHit{first_record + relative_record, record.logical_id, found_offset});
            }
        }
    }
    return hits;
}

bool import_zmdoc_corpus(
    const std::filesystem::path& corpus_path,
    const std::filesystem::path& store_root,
    StoreConfig config,
    StoreStats* stats,
    std::string* error) {
    std::ifstream corpus(corpus_path, std::ios::binary);
    if (!corpus) {
        *error = "cannot open ZMDOC corpus";
        return false;
    }
    std::array<std::byte, 64> header{};
    if (!read_exact(corpus, header)) {
        *error = "truncated ZMDOC header";
        return false;
    }
    for (std::size_t index = 0U; index < kCorpusMagic.size(); ++index) {
        if (std::to_integer<unsigned char>(header[index]) != static_cast<unsigned char>(kCorpusMagic[index])) {
            *error = "invalid ZMDOC magic";
            return false;
        }
    }
    const auto logical_bytes = read_le<std::uint64_t>(header, 8U);
    const auto records = read_le<std::uint64_t>(header, 16U);
    const auto nodes = read_le<std::uint64_t>(header, 24U);
    const auto styles = read_le<std::uint64_t>(header, 32U);
    const auto resources = read_le<std::uint64_t>(header, 40U);
    const auto largest = read_le<std::uint64_t>(header, 48U);
    if (!logical_bytes || !records || !nodes || !styles || !resources || !largest) {
        *error = "invalid ZMDOC header fields";
        return false;
    }
    StoreWriter writer(store_root, config);
    std::array<std::byte, 12> record_header{};
    std::array<std::byte, kIoWindowBytes> buffer{};
    for (std::uint64_t index = 0U; index < *records; ++index) {
        if (!read_exact(corpus, record_header)) {
            *error = "truncated ZMDOC record header";
            return false;
        }
        const auto logical_id = read_le<std::uint64_t>(record_header, 0U);
        const auto length32 = read_le<std::uint32_t>(record_header, 8U);
        if (!logical_id || !length32) {
            *error = "invalid ZMDOC record header";
            return false;
        }
        std::uint64_t remaining = *length32;
        if (!writer.append_stream(*logical_id, remaining, [&corpus, &buffer, &remaining](std::span<std::byte> destination) -> std::size_t {
                const std::size_t amount = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, std::min<std::size_t>(destination.size(), buffer.size())));
                if (amount == 0U) {
                    return 0U;
                }
                corpus.read(reinterpret_cast<char*>(destination.data()), static_cast<std::streamsize>(amount));
                const std::size_t received = static_cast<std::size_t>(corpus.gcount());
                remaining -= static_cast<std::uint64_t>(received);
                return received;
            }, error)) {
            return false;
        }
    }
    if (corpus.peek() != std::char_traits<char>::eof()) {
        *error = "ZMDOC corpus has trailing bytes";
        return false;
    }
    CorpusMetadata metadata{*logical_bytes, *records, *nodes, *styles, *resources, *largest};
    return writer.finalize(metadata, stats, error);
}

std::string stats_json(const StoreStats& stats) {
    std::ostringstream output;
    output << "{\"logical_utf8_bytes\":" << stats.corpus.logical_utf8_bytes
           << ",\"logical_records\":" << stats.corpus.logical_records
           << ",\"logical_nodes\":" << stats.corpus.logical_nodes
           << ",\"style_runs\":" << stats.corpus.style_runs
           << ",\"resource_references\":" << stats.corpus.resource_references
           << ",\"largest_record_bytes\":" << stats.corpus.largest_record_bytes
           << ",\"segment_count\":" << stats.segment_count
           << ",\"chunk_count\":" << stats.chunk_count
           << ",\"search_block_count\":" << stats.search_block_count
           << ",\"physical_bytes\":" << stats.physical_bytes
           << ",\"payload_sha256\":\"" << escape_json(stats.payload_sha256) << "\"}";
    return output.str();
}

} // namespace zevryon::massivedoc
