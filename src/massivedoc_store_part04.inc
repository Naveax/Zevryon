            return false;
        }
        std::uint64_t segment_offset = chunk.offset;
        std::uint64_t remaining = chunk.length;
        while (remaining != 0U) {
            const std::size_t amount = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, buffer.size()));
            if (!impl_->read_segment_exact(
                    chunk.segment_id,
                    segment_offset,
                    std::span<std::byte>(buffer.data(), amount),
                    error)) {
                *error = "cannot read record segment through immutable block cache: " + *error;
                return false;
            }
            if (!consumer(std::span<const std::byte>(buffer.data(), amount))) {
                return true;
            }
            segment_offset += static_cast<std::uint64_t>(amount);
            read_total += static_cast<std::uint64_t>(amount);
            remaining -= static_cast<std::uint64_t>(amount);
        }
    }
    if (read_total != record.length) {
        *error = "record length mismatch";
        return false;
    }
    return true;
}

bool StoreReader::read_record_slice(
    std::uint64_t record_index,
    std::uint64_t byte_offset,
    std::size_t max_bytes,
    std::vector<std::byte>* output,
    std::string* error) const {
    if (output == nullptr || error == nullptr) {
        return false;
    }
    output->clear();
    if (!impl_->opened) {
        *error = "store is not open";
        return false;
    }
    RecordDescriptor record;
    if (!impl_->read_record_descriptor(record_index, &record, error)) {
        return false;
    }
    if (byte_offset > record.length) {
        *error = "record slice offset out of range";
        return false;
    }
    const std::uint64_t available = record.length - byte_offset;
    const std::size_t wanted = static_cast<std::size_t>(std::min<std::uint64_t>(available, max_bytes));
    if (!materialized_slice_request_fits_address_space(wanted)) {
        *error = "record slice exceeds the process address-space materialization cap";
        return false;
    }
    output->reserve(wanted);
    std::uint64_t logical_position = 0U;
    std::size_t remaining_output = wanted;
    std::vector<std::byte> buffer(impl_->read_config.io_window_bytes);
    for (std::uint32_t relative = 0U; relative < record.chunk_count && remaining_output != 0U; ++relative) {
        ChunkDescriptor chunk;
        if (!impl_->read_chunk_descriptor(record.first_chunk + relative, &chunk, error)) {
            return false;
        }
        if (chunk.length > std::numeric_limits<std::uint64_t>::max() - logical_position) {
            *error = "record slice chunk range overflow";
            return false;
        }
        const std::uint64_t chunk_end = logical_position + chunk.length;
        if (byte_offset >= chunk_end) {
            logical_position = chunk_end;
            continue;
        }
        const std::uint64_t offset_in_chunk =
            byte_offset > logical_position ? byte_offset - logical_position : 0U;
        if (offset_in_chunk > chunk.length ||
            chunk.offset > std::numeric_limits<std::uint64_t>::max() - offset_in_chunk) {
            *error = "record slice segment offset overflow";
            return false;
        }
        std::uint64_t segment_offset = chunk.offset + offset_in_chunk;
        std::uint64_t chunk_remaining = chunk.length - offset_in_chunk;
        while (chunk_remaining != 0U && remaining_output != 0U) {
            const std::size_t amount = std::min<std::size_t>(
                remaining_output,
                static_cast<std::size_t>(
                    std::min<std::uint64_t>(chunk_remaining, buffer.size())));
            if (!impl_->read_segment_exact(
                    chunk.segment_id,
                    segment_offset,
                    std::span<std::byte>(buffer.data(), amount),
                    error)) {
                *error = "cannot read record slice through immutable block cache: " + *error;
                return false;
            }
            output->insert(
                output->end(),
                buffer.begin(),
                buffer.begin() + static_cast<std::ptrdiff_t>(amount));
            segment_offset += static_cast<std::uint64_t>(amount);
            remaining_output -= amount;
            chunk_remaining -= static_cast<std::uint64_t>(amount);
        }
        logical_position = chunk_end;
    }
    if (output->size() != wanted) {
        *error = "record slice length mismatch";
        return false;
    }
    return true;
}

bool StoreReader::verify(std::string* error) const {
    if (!impl_->opened) {
        *error = "store is not open";
        return false;
    }
    Sha256 sha;
    for (std::uint64_t index = 0U; index < impl_->stats.corpus.logical_records; ++index) {
        RecordDescriptor descriptor;
        if (!impl_->read_record_descriptor(index, &descriptor, error)) {
            return false;
        }
        std::uint32_t crc = 0U;
        std::uint64_t length = 0U;
        if (!read_record(index, [&sha, &crc, &length](std::span<const std::byte> bytes) {
                sha.update(bytes);
                crc = crc32_update(crc, bytes);
                length += static_cast<std::uint64_t>(bytes.size());
                return true;
            }, error)) {
            return false;
        }
        if (length != descriptor.length || crc != descriptor.crc32) {
            *error = "record integrity mismatch at index " + std::to_string(index);
            return false;
        }
    }
    if (sha.finish() != impl_->expected_sha) {
        *error = "payload SHA-256 mismatch";
        return false;
    }
    return true;
}

bool StoreReader::export_payload(const std::filesystem::path& output, std::string* error) const {
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    if (!stream) {
        *error = "cannot create export file";
        return false;
    }
    for (std::uint64_t index = 0U; index < impl_->stats.corpus.logical_records; ++index) {
        if (!read_record(index, [&stream](std::span<const std::byte> bytes) {
                return write_all(stream, bytes);
            }, error)) {
            return false;
        }
    }
    stream.flush();
    if (!stream) {
        *error = "failed to flush export file";
        return false;
    }
    return true;
}

std::vector<SearchHit> StoreReader::find(
    std::string_view query,
    std::size_t max_hits,
    std::string* error) const {
    return find_bounded(query, max_hits, {}, nullptr, error);
}

std::vector<SearchHit> StoreReader::find_bounded(
    std::string_view query,
    std::size_t max_hits,
    const SearchCancellationCheck& cancelled,
    SearchExecutionStats* execution_stats,
    std::string* error) const {
    if (execution_stats != nullptr) {
        *execution_stats = {};
    }
    std::vector<SearchHit> hits;
    if (!impl_->opened) {
        *error = "store is not open";
        return hits;
    }
    error->clear();
    if (query.empty() || max_hits == 0U) {
        return hits;
    }

    const auto cancellation_requested = [&cancelled]() {
        return static_cast<bool>(cancelled) && cancelled();
    };
    const auto mark_cancelled = [execution_stats, error]() {
        if (execution_stats != nullptr) {
            execution_stats->cancelled = true;
        }
        *error = "search cancelled";
    };

    const auto query_bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(query.data()),
        query.size());
    std::vector<std::byte> io_buffer(impl_->read_config.io_window_bytes);

    const auto scan_record = [this, query, query_bytes, &io_buffer, execution_stats, error](
                                 std::uint64_t record_index,
                                 std::vector<SearchHit>* output) -> bool {
        if (execution_stats != nullptr) {
            ++execution_stats->exact_records_scanned;
        }
        RecordDescriptor record;
        if (!impl_->read_record_descriptor(record_index, &record, error)) {
            return false;
        }
        std::vector<std::byte> carry;
        carry.reserve(query.size() > 1U ? query.size() - 1U : 0U);
        std::uint64_t processed = 0U;
        bool found = false;
        std::uint64_t found_offset = 0U;

        for (std::uint32_t relative_chunk = 0U;
             relative_chunk < record.chunk_count && !found;
             ++relative_chunk) {
            if (record.first_chunk >
                std::numeric_limits<std::uint64_t>::max() - relative_chunk) {
                *error = "search record chunk index overflow";
                return false;
            }
            ChunkDescriptor chunk;
            if (!impl_->read_chunk_descriptor(
                    record.first_chunk + relative_chunk,
                    &chunk,
                    error)) {
                return false;
            }

            std::uint64_t segment_offset = chunk.offset;
            std::uint64_t remaining = chunk.length;
            while (remaining != 0U && !found) {
                const std::size_t amount = static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining, io_buffer.size()));
                if (!impl_->read_segment_exact(
                        chunk.segment_id,
                        segment_offset,
                        std::span<std::byte>(io_buffer.data(), amount),
                        error)) {
                    *error = "cannot read candidate segment through immutable block cache: " + *error;
                    return false;
                }
                std::vector<std::byte> window;
                window.reserve(carry.size() + amount);
                window.insert(window.end(), carry.begin(), carry.end());
                window.insert(
                    window.end(),
                    io_buffer.begin(),
                    io_buffer.begin() + static_cast<std::ptrdiff_t>(amount));
                ExactByteMatchStats match_stats;
                const std::size_t local = find_exact_bytes(
                    std::span<const std::byte>(window.data(), window.size()),
                    query_bytes,
                    &match_stats);
                if (execution_stats != nullptr) {
                    execution_stats->exact_simd_batches += match_stats.simd_batches;
                    execution_stats->exact_scalar_candidates += match_stats.scalar_candidates;
                    execution_stats->exact_compares += match_stats.exact_compares;
                }
                if (local != kExactByteMatchNotFound) {
                    if (processed < static_cast<std::uint64_t>(carry.size())) {
                        *error = "search carry accounting underflow";
                        return false;
                    }
                    found_offset =
                        processed - static_cast<std::uint64_t>(carry.size()) +
                        static_cast<std::uint64_t>(local);
                    found = true;
                    break;
                }
                processed += static_cast<std::uint64_t>(amount);
                segment_offset += static_cast<std::uint64_t>(amount);
                remaining -= static_cast<std::uint64_t>(amount);
                const std::size_t keep = std::min<std::size_t>(
                    query.size() > 1U ? query.size() - 1U : 0U,
                    window.size());
                carry.assign(
                    window.end() - static_cast<std::ptrdiff_t>(keep),
                    window.end());
            }
        }
        if (found) {
            output->push_back(SearchHit{record_index, record.logical_id, found_offset});
        }
        return true;
    };

    // Derived trigram/Bloom data is an optional acceleration layer. User cancellation
    // is not a derived-data failure and therefore must never trigger legacy fallback.
    if (query.size() >= 3U) {
        TrigramIndexConfig trigram_config;
        trigram_config.io_window_bytes = impl_->read_config.io_window_bytes;
        TrigramIndexReader trigram(impl_->root, trigram_config);
        std::string trigram_error;
        if (trigram.open(impl_->expected_sha, &trigram_error) &&
            trigram.block_count() == impl_->stats.corpus.logical_records) {
            if (execution_stats != nullptr) {
                execution_stats->used_trigram = true;
            }
            std::vector<SearchHit> accelerated_hits;
            bool source_ok = true;
            TrigramQueryStats query_stats;
            const bool candidate_ok = trigram.visit_candidate_blocks(
                query,
                [&](std::uint64_t record_index) {
                    if (execution_stats != nullptr) {
                        ++execution_stats->trigram_candidate_blocks;
                    }
                    if (record_index >= impl_->stats.corpus.logical_records) {
                        *error = "trigram candidate record index out of range";
                        source_ok = false;
                        return false;
                    }
                    if (!scan_record(record_index, &accelerated_hits)) {
                        source_ok = false;
                        return false;
                    }
                    return accelerated_hits.size() < max_hits;
                },
                cancellation_requested,
                &query_stats,
                &trigram_error);
            if (query_stats.cancelled) {
                mark_cancelled();
                return {};
            }
            if (!source_ok) {
                return {};
            }
            if (candidate_ok) {
                error->clear();
                return accelerated_hits;
            }
            if (execution_stats != nullptr) {
                execution_stats->fell_back_from_trigram = true;
            }
            // A checksum/framing/read error in derived data cannot make partial
            // accelerated results authoritative. Start the exact legacy search over.
            error->clear();
        } else {
            if (execution_stats != nullptr) {
                execution_stats->fell_back_from_trigram = true;
            }
        }
    }

    BoundedPositionalReader signatures(
        impl_->root / "search.bgm",
        impl_->read_config.io_window_bytes);
    if (!signatures.open(error)) {
        *error = "cannot open search index positional reader: " + *error;
        return hits;
    }

    std::array<std::byte, kBigramSignatureBytes> signature{};
    for (std::uint64_t block = 0U;
         block < impl_->stats.search_block_count && hits.size() < max_hits;
         ++block) {
        if (cancellation_requested()) {
            mark_cancelled();
            return {};
        }
        if (execution_stats != nullptr) {
            ++execution_stats->legacy_blocks_checked;
        }
        if (block > std::numeric_limits<std::uint64_t>::max() / kBigramSignatureBytes) {
            *error = "search signature offset overflow";
            return {};
        }
        const std::uint64_t signature_offset = block * kBigramSignatureBytes;
        if (!signatures.read_exact_at(signature_offset, signature, error)) {
            *error = "cannot read search signature positionally: " + *error;
            return {};
        }

        bool candidate = true;
        if (query.size() >= 2U) {
            for (std::size_t index = 1U; index < query.size(); ++index) {
                if (!has_bigram(
                        signature,
                        static_cast<unsigned char>(query[index - 1U]),
                        static_cast<unsigned char>(query[index]))) {
                    candidate = false;
                    break;
                }
            }
        }
        if (!candidate) {
            continue;
        }

        const std::uint64_t records_per_block =
            static_cast<std::uint64_t>(impl_->config.records_per_search_block);
        if (records_per_block == 0U ||
            block > std::numeric_limits<std::uint64_t>::max() / records_per_block) {
            *error = "search record block offset overflow";
            return {};
        }
        const std::uint64_t first_record = block * records_per_block;
        if (first_record >= impl_->stats.corpus.logical_records) {
            break;
        }
        const std::uint64_t records_remaining =
            impl_->stats.corpus.logical_records - first_record;
        const std::uint64_t record_count =
            std::min(records_remaining, records_per_block);
        const std::uint64_t end_record = first_record + record_count;

        for (std::uint64_t record_index = first_record;
             record_index < end_record && hits.size() < max_hits;
             ++record_index) {
            if (!scan_record(record_index, &hits)) {
                return {};
            }
        }
    }
    return hits;
}
