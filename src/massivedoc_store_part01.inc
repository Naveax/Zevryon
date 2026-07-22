    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        value |= static_cast<T>(std::to_integer<unsigned char>(bytes[offset + index])) << (index * 8U);
    }
    return value;
}

bool write_all(std::ostream& stream, std::span<const std::byte> bytes) {
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream);
}

bool read_exact(std::istream& stream, std::span<std::byte> bytes) {
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return stream.gcount() == static_cast<std::streamsize>(bytes.size());
}

std::filesystem::path segment_path(const std::filesystem::path& root, std::uint32_t segment) {
    std::ostringstream name;
    name << "segment-" << std::setw(8) << std::setfill('0') << segment << ".bin";
    return root / "segments" / name.str();
}

void set_bigram(std::array<std::byte, kBigramSignatureBytes>& signature, unsigned char first, unsigned char second) {
    const std::uint32_t value = (static_cast<std::uint32_t>(first) << 8U) | static_cast<std::uint32_t>(second);
    const std::size_t byte_index = static_cast<std::size_t>(value >> 3U);
    const unsigned int bit_index = value & 7U;
    signature[byte_index] |= static_cast<std::byte>(1U << bit_index);
}

bool has_bigram(std::span<const std::byte> signature, unsigned char first, unsigned char second) {
    const std::uint32_t value = (static_cast<std::uint32_t>(first) << 8U) | static_cast<std::uint32_t>(second);
    const std::size_t byte_index = static_cast<std::size_t>(value >> 3U);
    const unsigned int bit_index = value & 7U;
    return (std::to_integer<unsigned int>(signature[byte_index]) & (1U << bit_index)) != 0U;
}

struct RecordDescriptor {
    std::uint64_t logical_id{0};
    std::uint64_t first_chunk{0};
    std::uint64_t length{0};
    std::uint32_t chunk_count{0};
    std::uint32_t crc32{0};
};

struct ChunkDescriptor {
    std::uint32_t segment_id{0};
    std::uint64_t offset{0};
    std::uint64_t length{0};
};

std::vector<std::byte> serialize_record(const RecordDescriptor& descriptor) {
    std::vector<std::byte> bytes;
    bytes.reserve(kRecordDescriptorBytes);
    append_le(bytes, descriptor.logical_id);
    append_le(bytes, descriptor.first_chunk);
    append_le(bytes, descriptor.length);
    append_le(bytes, descriptor.chunk_count);
    append_le(bytes, descriptor.crc32);
    return bytes;
}

std::optional<RecordDescriptor> parse_record(std::span<const std::byte> bytes) {
    if (bytes.size() != kRecordDescriptorBytes) {
        return std::nullopt;
    }
    RecordDescriptor descriptor;
    const auto logical_id = read_le<std::uint64_t>(bytes, 0U);
    const auto first_chunk = read_le<std::uint64_t>(bytes, 8U);
    const auto length = read_le<std::uint64_t>(bytes, 16U);
    const auto chunk_count = read_le<std::uint32_t>(bytes, 24U);
    const auto crc = read_le<std::uint32_t>(bytes, 28U);
    if (!logical_id || !first_chunk || !length || !chunk_count || !crc) {
        return std::nullopt;
    }
    descriptor.logical_id = *logical_id;
    descriptor.first_chunk = *first_chunk;
    descriptor.length = *length;
    descriptor.chunk_count = *chunk_count;
    descriptor.crc32 = *crc;
    return descriptor;
}

std::vector<std::byte> serialize_chunk(const ChunkDescriptor& descriptor) {
    std::vector<std::byte> bytes;
    bytes.reserve(kChunkDescriptorBytes);
    append_le(bytes, descriptor.segment_id);
    append_le(bytes, std::uint32_t{0U});
    append_le(bytes, descriptor.offset);
    append_le(bytes, descriptor.length);
    return bytes;
}

std::optional<ChunkDescriptor> parse_chunk(std::span<const std::byte> bytes) {
    if (bytes.size() != kChunkDescriptorBytes) {
        return std::nullopt;
    }
    const auto segment_id = read_le<std::uint32_t>(bytes, 0U);
    const auto offset = read_le<std::uint64_t>(bytes, 8U);
    const auto length = read_le<std::uint64_t>(bytes, 16U);
    if (!segment_id || !offset || !length) {
        return std::nullopt;
    }
    return ChunkDescriptor{*segment_id, *offset, *length};
}

bool seek_stream(std::istream& stream, std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return false;
    }
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    return static_cast<bool>(stream);
}

std::uint64_t directory_size(const std::filesystem::path& root) {
    std::error_code error;
    std::uint64_t total = 0U;
    for (std::filesystem::recursive_directory_iterator iterator(root, error), end; iterator != end && !error; iterator.increment(error)) {
        if (iterator->is_regular_file(error)) {
            total += iterator->file_size(error);
        }
    }
    return total;
}

std::string escape_json(std::string_view input) {
    std::ostringstream output;
    for (const char raw_character : input) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20U) {
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned int>(character) << std::dec;
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    return output.str();
}

} // namespace

struct StoreWriter::Impl {
    std::filesystem::path root;
    StoreConfig config;
    std::ofstream records;
    std::ofstream chunks;
    std::ofstream search;
    std::ofstream segment;
    std::uint32_t segment_id{0U};
    std::uint64_t segment_offset{0U};
    std::uint64_t record_count{0U};
    std::uint64_t chunk_count{0U};
    std::uint64_t logical_bytes{0U};
    std::uint64_t largest_record{0U};
    std::uint64_t search_blocks{0U};
    std::uint32_t records_in_block{0U};
    std::array<std::byte, kBigramSignatureBytes> signature{};
    Sha256 sha;
    bool finalized{false};

    bool open_segment(std::string* error) {
        if (segment.is_open()) {
            segment.close();
        }
        const auto path = segment_path(root, segment_id);
        segment.open(path, std::ios::binary | std::ios::trunc);
        if (!segment) {
            *error = "cannot create segment: " + path.string();
            return false;
        }
        segment_offset = 0U;
        return true;
    }

    bool flush_search_block(std::string* error) {
        if (records_in_block == 0U) {
            return true;
        }
        if (!write_all(search, signature)) {
            *error = "failed to write search block";
            return false;
