    std::ofstream stream(impl_->root / "manifest.zmd", std::ios::binary | std::ios::trunc);
    if (!stream || !write_all(stream, manifest)) {
        *error = "failed to write store manifest";
        return false;
    }
    stream.flush();
    impl_->finalized = true;
    if (stats != nullptr) {
        stats->corpus = metadata;
        stats->segment_count = static_cast<std::uint64_t>(impl_->segment_id) + 1U;
        stats->chunk_count = impl_->chunk_count;
        stats->search_block_count = impl_->search_blocks;
        stats->payload_sha256 = hex_digest(digest);
        stats->physical_bytes = directory_size(impl_->root);
    }
    return true;
}

struct StoreReader::Impl {
    std::filesystem::path root;
    StoreStats stats;
    StoreConfig config;
    std::array<std::uint8_t, 32> expected_sha{};
    bool opened{false};

    bool read_record_descriptor(std::uint64_t index, RecordDescriptor* descriptor, std::string* error) const {
        if (index >= stats.corpus.logical_records) {
            *error = "record index out of range";
            return false;
        }
        std::ifstream stream(root / "records.idx", std::ios::binary);
        const std::uint64_t offset = index * kRecordDescriptorBytes;
        if (!stream || !seek_stream(stream, offset)) {
            *error = "cannot seek record index";
            return false;
        }
        std::array<std::byte, kRecordDescriptorBytes> bytes{};
        if (!read_exact(stream, bytes)) {
            *error = "cannot read record descriptor";
            return false;
        }
        const auto parsed = parse_record(bytes);
        if (!parsed) {
            *error = "invalid record descriptor";
            return false;
        }
        *descriptor = *parsed;
        return true;
    }

    bool read_chunk_descriptor(std::uint64_t index, ChunkDescriptor* descriptor, std::string* error) const {
        if (index >= stats.chunk_count) {
            *error = "chunk index out of range";
            return false;
        }
        std::ifstream stream(root / "chunks.idx", std::ios::binary);
        const std::uint64_t offset = index * kChunkDescriptorBytes;
        if (!stream || !seek_stream(stream, offset)) {
            *error = "cannot seek chunk index";
            return false;
        }
        std::array<std::byte, kChunkDescriptorBytes> bytes{};
        if (!read_exact(stream, bytes)) {
            *error = "cannot read chunk descriptor";
            return false;
        }
        const auto parsed = parse_chunk(bytes);
        if (!parsed) {
            *error = "invalid chunk descriptor";
            return false;
        }
        *descriptor = *parsed;
        return true;
    }
};

StoreReader::StoreReader(const std::filesystem::path& root) : impl_(new Impl{}) {
    impl_->root = root;
}

StoreReader::~StoreReader() {
    delete impl_;
}

bool StoreReader::open(std::string* error) {
    std::ifstream stream(impl_->root / "manifest.zmd", std::ios::binary);
    if (!stream) {
        *error = "cannot open store manifest";
        return false;
    }
    std::array<std::byte, kManifestBytes> manifest{};
    if (!read_exact(stream, manifest)) {
        *error = "truncated store manifest";
        return false;
    }
    for (std::size_t index = 0U; index < kManifestMagic.size(); ++index) {
        if (std::to_integer<unsigned char>(manifest[index]) != static_cast<unsigned char>(kManifestMagic[index])) {
            *error = "invalid store manifest magic";
            return false;
        }
    }
    const auto version = read_le<std::uint32_t>(manifest, 8U);
    const auto header_bytes = read_le<std::uint32_t>(manifest, 12U);
    if (!version || !header_bytes || *version != kFormatVersion || *header_bytes != kManifestBytes) {
        *error = "unsupported store manifest version";
        return false;
    }
    const auto expected_crc = read_le<std::uint32_t>(manifest, kManifestBytes - 4U);
    const std::uint32_t actual_crc = crc32_update(0U, std::span<const std::byte>(manifest.data(), kManifestBytes - 4U));
    if (!expected_crc || *expected_crc != actual_crc) {
        *error = "store manifest CRC mismatch";
        return false;
    }
    const auto segment_bytes = read_le<std::uint64_t>(manifest, 16U);
    const auto records_per_block = read_le<std::uint32_t>(manifest, 24U);
    const auto logical_bytes = read_le<std::uint64_t>(manifest, 32U);
    const auto logical_records = read_le<std::uint64_t>(manifest, 40U);
    const auto logical_nodes = read_le<std::uint64_t>(manifest, 48U);
    const auto style_runs = read_le<std::uint64_t>(manifest, 56U);
    const auto resources = read_le<std::uint64_t>(manifest, 64U);
    const auto largest_record = read_le<std::uint64_t>(manifest, 72U);
    const auto segments = read_le<std::uint64_t>(manifest, 80U);
    const auto chunks = read_le<std::uint64_t>(manifest, 88U);
    const auto search_blocks = read_le<std::uint64_t>(manifest, 96U);
    if (!segment_bytes || !records_per_block || !logical_bytes || !logical_records || !logical_nodes || !style_runs || !resources ||
        !largest_record || !segments || !chunks || !search_blocks) {
        *error = "invalid store manifest fields";
        return false;
    }
    impl_->config.segment_bytes = *segment_bytes;
    impl_->config.records_per_search_block = *records_per_block;
    impl_->stats.corpus = CorpusMetadata{*logical_bytes, *logical_records, *logical_nodes, *style_runs, *resources, *largest_record};
    impl_->stats.segment_count = *segments;
    impl_->stats.chunk_count = *chunks;
    impl_->stats.search_block_count = *search_blocks;
    for (std::size_t index = 0U; index < impl_->expected_sha.size(); ++index) {
        impl_->expected_sha[index] = std::to_integer<std::uint8_t>(manifest[104U + index]);
    }
    impl_->stats.payload_sha256 = hex_digest(impl_->expected_sha);
    impl_->stats.physical_bytes = directory_size(impl_->root);
    std::error_code size_error;
    const std::uint64_t records_size = std::filesystem::file_size(impl_->root / "records.idx", size_error);
    if (size_error || records_size != impl_->stats.corpus.logical_records * kRecordDescriptorBytes) {
        *error = "record index size mismatch";
        return false;
    }
    const std::uint64_t chunks_size = std::filesystem::file_size(impl_->root / "chunks.idx", size_error);
    if (size_error || chunks_size != impl_->stats.chunk_count * kChunkDescriptorBytes) {
        *error = "chunk index size mismatch";
        return false;
    }
    const std::uint64_t search_size = std::filesystem::file_size(impl_->root / "search.bgm", size_error);
    if (size_error || search_size != impl_->stats.search_block_count * kBigramSignatureBytes) {
        *error = "search index size mismatch";
        return false;
    }
    for (std::uint64_t segment = 0U; segment < impl_->stats.segment_count; ++segment) {
        if (segment > std::numeric_limits<std::uint32_t>::max() ||
            !std::filesystem::is_regular_file(segment_path(impl_->root, static_cast<std::uint32_t>(segment)), size_error) || size_error) {
            *error = "missing segment file";
            return false;
        }
    }
    impl_->opened = true;
    return true;
}

const StoreStats& StoreReader::stats() const noexcept {
    return impl_->stats;
}

bool StoreReader::read_record(
    std::uint64_t record_index,
    const std::function<bool(std::span<const std::byte>)>& consumer,
    std::string* error) const {
    if (!impl_->opened) {
        *error = "store is not open";
        return false;
    }
    RecordDescriptor record;
    if (!impl_->read_record_descriptor(record_index, &record, error)) {
        return false;
    }
    std::vector<std::byte> buffer(kIoWindowBytes);
    std::uint64_t read_total = 0U;
    for (std::uint32_t relative = 0U; relative < record.chunk_count; ++relative) {
        ChunkDescriptor chunk;
        if (!impl_->read_chunk_descriptor(record.first_chunk + relative, &chunk, error)) {