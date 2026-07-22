            return false;
        }
        std::ifstream segment(segment_path(impl_->root, chunk.segment_id), std::ios::binary);
        if (!segment || !seek_stream(segment, chunk.offset)) {
            *error = "cannot open or seek segment";
            return false;
        }
        std::uint64_t remaining = chunk.length;
        while (remaining != 0U) {
            const std::size_t amount = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            if (!read_exact(segment, std::span<std::byte>(buffer.data(), amount))) {
                *error = "truncated segment payload";
                return false;
            }
            if (!consumer(std::span<const std::byte>(buffer.data(), amount))) {
                return true;
            }
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
    output->reserve(wanted);
    std::uint64_t logical_position = 0U;
    std::size_t remaining_output = wanted;
    std::array<std::byte, kIoWindowBytes> buffer{};
    for (std::uint32_t relative = 0U; relative < record.chunk_count && remaining_output != 0U; ++relative) {
        ChunkDescriptor chunk;
        if (!impl_->read_chunk_descriptor(record.first_chunk + relative, &chunk, error)) {
            return false;
        }
        const std::uint64_t chunk_end = logical_position + chunk.length;
        if (byte_offset >= chunk_end) {
            logical_position = chunk_end;
            continue;
        }
        const std::uint64_t offset_in_chunk = byte_offset > logical_position ? byte_offset - logical_position : 0U;
        std::ifstream segment(segment_path(impl_->root, chunk.segment_id), std::ios::binary);
        if (!segment || !seek_stream(segment, chunk.offset + offset_in_chunk)) {
            *error = "cannot open or seek record slice segment";
            return false;
        }
        std::uint64_t chunk_remaining = chunk.length - offset_in_chunk;
        while (chunk_remaining != 0U && remaining_output != 0U) {
            const std::size_t amount = std::min<std::size_t>(
                remaining_output,
                static_cast<std::size_t>(std::min<std::uint64_t>(chunk_remaining, buffer.size())));
            if (!read_exact(segment, std::span<std::byte>(buffer.data(), amount))) {
                *error = "truncated record slice segment";
                return false;
            }
            output->insert(output->end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(amount));
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

std::vector<SearchHit> StoreReader::find(std::string_view query, std::size_t max_hits, std::string* error) const {
    std::vector<SearchHit> hits;
    if (!impl_->opened) {
        *error = "store is not open";
        return hits;
    }
    if (query.empty() || max_hits == 0U) {
        return hits;
    }
    std::ifstream signatures(impl_->root / "search.bgm", std::ios::binary);
    std::ifstream record_index(impl_->root / "records.idx", std::ios::binary);
    std::ifstream chunk_index(impl_->root / "chunks.idx", std::ios::binary);
    if (!signatures || !record_index || !chunk_index) {
        *error = "cannot open search or descriptor index";
        return hits;
    }
    std::array<std::byte, kBigramSignatureBytes> signature{};
    const auto query_bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(query.data()), query.size());
    std::array<std::byte, kIoWindowBytes> io_buffer{};
    std::ifstream segment_stream;
    std::uint32_t open_segment = std::numeric_limits<std::uint32_t>::max();

    for (std::uint64_t block = 0U; block < impl_->stats.search_block_count && hits.size() < max_hits; ++block) {
        if (!read_exact(signatures, signature)) {
            *error = "truncated search index";
            return {};
        }
        bool candidate = true;
        if (query.size() >= 2U) {
            for (std::size_t index = 1U; index < query.size(); ++index) {
                if (!has_bigram(signature, static_cast<unsigned char>(query[index - 1U]),
                                static_cast<unsigned char>(query[index]))) {
                    candidate = false;
                    break;
                }
            }
        }
        if (!candidate) {
            continue;
        }

        const std::uint64_t first_record = block * impl_->config.records_per_search_block;
        const std::uint64_t end_record = std::min<std::uint64_t>(
