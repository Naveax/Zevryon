        }
        signature.fill(std::byte{0});
        records_in_block = 0U;
        ++search_blocks;
        return true;
    }
};

StoreWriter::StoreWriter(const std::filesystem::path& root, StoreConfig config) : impl_(new Impl{}) {
    impl_->root = root;
    impl_->config = config;
    if (impl_->config.segment_bytes == 0U) {
        impl_->config.segment_bytes = kDefaultSegmentBytes;
    }
    if (impl_->config.records_per_search_block == 0U) {
        impl_->config.records_per_search_block = kDefaultRecordsPerSearchBlock;
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(root / "segments", error);
    if (error) {
        return;
    }
    impl_->records.open(root / "records.idx", std::ios::binary | std::ios::trunc);
    impl_->chunks.open(root / "chunks.idx", std::ios::binary | std::ios::trunc);
    impl_->search.open(root / "search.bgm", std::ios::binary | std::ios::trunc);
    std::string ignored;
    if (impl_->records && impl_->chunks && impl_->search) {
        (void)impl_->open_segment(&ignored);
    }
}

StoreWriter::~StoreWriter() {
    delete impl_;
}

bool StoreWriter::append(std::uint64_t logical_id, std::span<const std::byte> payload, std::string* error) {
    std::size_t offset = 0U;
    return append_stream(
        logical_id,
        static_cast<std::uint64_t>(payload.size()),
        [&payload, &offset](std::span<std::byte> destination) -> std::size_t {
            const std::size_t amount = std::min(destination.size(), payload.size() - offset);
            if (amount != 0U) {
                std::memcpy(destination.data(), payload.data() + offset, amount);
                offset += amount;
            }
            return amount;
        },
        error);
}

bool StoreWriter::append_stream(
    std::uint64_t logical_id,
    std::uint64_t length,
    const std::function<std::size_t(std::span<std::byte>)>& reader,
    std::string* error) {
    if (impl_ == nullptr || impl_->finalized) {
        *error = "store writer is unavailable or finalized";
        return false;
    }
    if (!impl_->records || !impl_->chunks || !impl_->search || !impl_->segment) {
        *error = "store files are not open";
        return false;
    }
    const std::uint64_t first_chunk = impl_->chunk_count;
    std::uint64_t remaining = length;
    std::uint32_t chunk_count = 0U;
    std::uint32_t crc = 0U;
    std::vector<std::byte> buffer(kIoWindowBytes);
    bool have_previous = false;
    unsigned char previous = 0U;
    while (remaining != 0U) {
        if (impl_->segment_offset == impl_->config.segment_bytes) {
            ++impl_->segment_id;
            if (!impl_->open_segment(error)) {
                return false;
            }
        }
        const std::uint64_t segment_remaining = impl_->config.segment_bytes - impl_->segment_offset;
        const std::uint64_t chunk_length = std::min(remaining, segment_remaining);
        const ChunkDescriptor chunk{impl_->segment_id, impl_->segment_offset, chunk_length};
        const auto chunk_bytes = serialize_chunk(chunk);
        if (!write_all(impl_->chunks, chunk_bytes)) {
            *error = "failed to write chunk descriptor";
            return false;
        }
        ++impl_->chunk_count;
        ++chunk_count;
        std::uint64_t chunk_remaining = chunk_length;
        while (chunk_remaining != 0U) {
            const std::size_t request = static_cast<std::size_t>(std::min<std::uint64_t>(chunk_remaining, buffer.size()));
            const std::size_t received = reader(std::span<std::byte>(buffer.data(), request));
            if (received == 0U || received > request) {
                *error = "record stream ended before declared length";
                return false;
            }
            const auto bytes = std::span<const std::byte>(buffer.data(), received);
            if (!write_all(impl_->segment, bytes)) {
                *error = "failed to write segment payload";
                return false;
            }
            crc = crc32_update(crc, bytes);
            impl_->sha.update(bytes);
            for (const std::byte raw : bytes) {
                const unsigned char current = std::to_integer<unsigned char>(raw);
                if (have_previous) {
                    set_bigram(impl_->signature, previous, current);
                }
                previous = current;
                have_previous = true;
            }
            impl_->segment_offset += static_cast<std::uint64_t>(received);
            impl_->logical_bytes += static_cast<std::uint64_t>(received);
            chunk_remaining -= static_cast<std::uint64_t>(received);
            remaining -= static_cast<std::uint64_t>(received);
        }
    }
    const RecordDescriptor descriptor{logical_id, first_chunk, length, chunk_count, crc};
    const auto record_bytes = serialize_record(descriptor);
    if (!write_all(impl_->records, record_bytes)) {
        *error = "failed to write record descriptor";
        return false;
    }
    ++impl_->record_count;
    impl_->largest_record = std::max(impl_->largest_record, length);
    ++impl_->records_in_block;
    if (impl_->records_in_block == impl_->config.records_per_search_block && !impl_->flush_search_block(error)) {
        return false;
    }
    return true;
}

bool StoreWriter::finalize(CorpusMetadata metadata, StoreStats* stats, std::string* error) {
    if (impl_ == nullptr || impl_->finalized) {
        *error = "store writer already finalized";
        return false;
    }
    if (!impl_->flush_search_block(error)) {
        return false;
    }
    impl_->records.flush();
    impl_->chunks.flush();
    impl_->search.flush();
    impl_->segment.flush();
    if (!impl_->records || !impl_->chunks || !impl_->search || !impl_->segment) {
        *error = "failed to flush store files";
        return false;
    }
    metadata.logical_utf8_bytes = impl_->logical_bytes;
    metadata.logical_records = impl_->record_count;
    metadata.largest_record_bytes = std::max(metadata.largest_record_bytes, impl_->largest_record);
    if (metadata.logical_nodes == 0U) {
        metadata.logical_nodes = impl_->record_count;
    }
    const auto digest = impl_->sha.finish();
    std::vector<std::byte> manifest;
    manifest.reserve(kManifestBytes);
    for (const char character : kManifestMagic) {
        manifest.push_back(static_cast<std::byte>(character));
    }
    append_le(manifest, kFormatVersion);
    append_le(manifest, std::uint32_t{kManifestBytes});
    append_le(manifest, impl_->config.segment_bytes);
    append_le(manifest, impl_->config.records_per_search_block);
    append_le(manifest, std::uint32_t{0U});
    append_le(manifest, metadata.logical_utf8_bytes);
    append_le(manifest, metadata.logical_records);
    append_le(manifest, metadata.logical_nodes);
    append_le(manifest, metadata.style_runs);
    append_le(manifest, metadata.resource_references);
    append_le(manifest, metadata.largest_record_bytes);
    append_le(manifest, static_cast<std::uint64_t>(impl_->segment_id) + 1U);
    append_le(manifest, impl_->chunk_count);
    append_le(manifest, impl_->search_blocks);
    for (const std::uint8_t byte : digest) {
        manifest.push_back(static_cast<std::byte>(byte));
    }
    while (manifest.size() + sizeof(std::uint32_t) < kManifestBytes) {
        manifest.push_back(std::byte{0});
    }
    const std::uint32_t manifest_crc = crc32_update(0U, manifest);
    append_le(manifest, manifest_crc);
    if (manifest.size() != kManifestBytes) {
        *error = "internal manifest size mismatch";
        return false;
    }
