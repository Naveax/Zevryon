#include "logical_node_source_map.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace zevryon::massivedoc {
namespace {

constexpr std::array<std::uint8_t, 8> kManifestMagic{
    'Z', 'V', 'N', 'S', 'R', 'C', '0', '1'};
constexpr std::uint32_t kFormatVersion = 1U;
constexpr std::uint64_t kHeadRecordSize = 16U;
constexpr std::uint64_t kPostingRecordSize = 32U;
constexpr std::uint64_t kManifestSize = 168U;
constexpr std::size_t kInitChunkRecords = 4096U;

bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool is_hex_sha(std::string_view value) {
    if (value.size() != 40U) {
        return false;
    }
    for (const char character : value) {
        const bool digit = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'f';
        const bool upper = character >= 'A' && character <= 'F';
        if (!digit && !lower && !upper) {
            return false;
        }
    }
    return true;
}

void append_u32(std::vector<std::uint8_t>* bytes, std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        bytes->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_u64(std::vector<std::uint8_t>* bytes, std::uint64_t value) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        bytes->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint32_t value = 0U;
    for (unsigned index = 0U; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

std::uint64_t read_u64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t value = 0U;
    for (unsigned index = 0U; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

void append_string_bytes(std::vector<std::uint8_t>* bytes, std::string_view value) {
    for (const char character : value) {
        bytes->push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
    }
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t byte : bytes) {
        crc ^= static_cast<std::uint32_t>(byte);
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

bool stream_count(std::size_t size, std::streamsize* value, std::string* error) {
    const auto maximum = static_cast<std::uintmax_t>(
        std::numeric_limits<std::streamsize>::max());
    if (static_cast<std::uintmax_t>(size) > maximum) {
        return fail(error, "logical node source-map I/O size exceeds stream limit");
    }
    *value = static_cast<std::streamsize>(size);
    return true;
}

bool stream_offset(std::uint64_t offset, std::streamoff* value, std::string* error) {
    const auto maximum = static_cast<std::uint64_t>(
        std::numeric_limits<std::streamoff>::max());
    if (offset > maximum) {
        return fail(error, "logical node source-map offset exceeds stream limit");
    }
    *value = static_cast<std::streamoff>(offset);
    return true;
}

bool seek_read(std::istream* stream, std::uint64_t offset, std::string* error) {
    std::streamoff converted = 0;
    if (!stream_offset(offset, &converted, error)) {
        return false;
    }
    stream->clear();
    stream->seekg(converted, std::ios::beg);
    if (!*stream) {
        return fail(error, "logical node source-map read seek failed");
    }
    return true;
}

bool seek_write(std::ostream* stream, std::uint64_t offset, std::string* error) {
    std::streamoff converted = 0;
    if (!stream_offset(offset, &converted, error)) {
        return false;
    }
    stream->clear();
    stream->seekp(converted, std::ios::beg);
    if (!*stream) {
        return fail(error, "logical node source-map write seek failed");
    }
    return true;
}

bool write_bytes(
    std::ostream* stream,
    std::span<const std::uint8_t> bytes,
    std::string* error) {
    std::streamsize count = 0;
    if (!stream_count(bytes.size(), &count, error)) {
        return false;
    }
    if (!bytes.empty()) {
        stream->write(reinterpret_cast<const char*>(bytes.data()), count);
    }
    if (!*stream) {
        return fail(error, "logical node source-map write failed");
    }
    return true;
}

bool read_bytes(
    std::istream* stream,
    std::span<std::uint8_t> bytes,
    std::string* error) {
    std::streamsize count = 0;
    if (!stream_count(bytes.size(), &count, error)) {
        return false;
    }
    if (!bytes.empty()) {
        stream->read(reinterpret_cast<char*>(bytes.data()), count);
    }
    if (!*stream) {
        return fail(error, "logical node source-map read failed or was truncated");
    }
    return true;
}

bool multiply_size(
    std::uint64_t count,
    std::uint64_t width,
    std::uint64_t* result,
    std::string* error) {
    if (width != 0U && count > std::numeric_limits<std::uint64_t>::max() / width) {
        return fail(error, "logical node source-map size overflow");
    }
    *result = count * width;
    return true;
}

bool require_file_size(
    const std::filesystem::path& path,
    std::uint64_t expected,
    std::string* error) {
    std::error_code fs_error;
    const std::uintmax_t actual = std::filesystem::file_size(path, fs_error);
    if (fs_error) {
        return fail(error, "cannot stat logical node source-map file: " + fs_error.message());
    }
    if (actual != static_cast<std::uintmax_t>(expected)) {
        return fail(error, "logical node source-map file size mismatch: " + path.string());
    }
    return true;
}

std::vector<std::uint8_t> encode_head(std::uint64_t posting_plus_one) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(kHeadRecordSize));
    append_u64(&bytes, posting_plus_one);
    append_u32(&bytes, 0U);
    append_u32(&bytes, crc32(std::span<const std::uint8_t>(bytes.data(), bytes.size())));
    return bytes;
}

bool decode_head(
    std::span<const std::uint8_t> bytes,
    std::uint64_t* posting_plus_one,
    std::string* error) {
    if (bytes.size() != static_cast<std::size_t>(kHeadRecordSize)) {
        return fail(error, "logical node source-map head size mismatch");
    }
    if (read_u32(bytes, 8U) != 0U || crc32(bytes.first(12U)) != read_u32(bytes, 12U)) {
        return fail(error, "logical node source-map head CRC/reserved mismatch");
    }
    *posting_plus_one = read_u64(bytes, 0U);
    return true;
}

struct Posting {
    std::uint64_t source_record_index{0U};
    std::uint64_t node_ordinal{0U};
    std::uint64_t next_posting_plus_one{0U};
};

std::vector<std::uint8_t> encode_posting(const Posting& posting) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(kPostingRecordSize));
    append_u64(&bytes, posting.source_record_index);
    append_u64(&bytes, posting.node_ordinal);
    append_u64(&bytes, posting.next_posting_plus_one);
    append_u32(&bytes, 0U);
    append_u32(&bytes, crc32(std::span<const std::uint8_t>(bytes.data(), bytes.size())));
    return bytes;
}

bool decode_posting(
    std::span<const std::uint8_t> bytes,
    Posting* posting,
    std::string* error) {
    if (bytes.size() != static_cast<std::size_t>(kPostingRecordSize)) {
        return fail(error, "logical node source-map posting size mismatch");
    }
    if (read_u32(bytes, 24U) != 0U || crc32(bytes.first(28U)) != read_u32(bytes, 28U)) {
        return fail(error, "logical node source-map posting CRC/reserved mismatch");
    }
    posting->source_record_index = read_u64(bytes, 0U);
    posting->node_ordinal = read_u64(bytes, 8U);
    posting->next_posting_plus_one = read_u64(bytes, 16U);
    return true;
}

std::vector<std::uint8_t> encode_manifest(const LogicalNodeSourceMapManifest& manifest) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(kManifestSize));
    bytes.insert(bytes.end(), kManifestMagic.begin(), kManifestMagic.end());
    append_u32(&bytes, manifest.format_version);
    append_u32(&bytes, 0U);
    append_u64(&bytes, manifest.source_record_count);
    append_u64(&bytes, manifest.node_count);
    append_u64(&bytes, manifest.posting_count);
    append_string_bytes(&bytes, manifest.candidate_commit);
    append_string_bytes(&bytes, manifest.candidate_tree);
    bytes.insert(bytes.end(), manifest.source_sha256.begin(), manifest.source_sha256.end());
    append_u32(&bytes, static_cast<std::uint32_t>(kHeadRecordSize));
    append_u32(&bytes, static_cast<std::uint32_t>(kPostingRecordSize));
    append_u32(&bytes, 0U);
    append_u32(&bytes, crc32(std::span<const std::uint8_t>(bytes.data(), bytes.size())));
    return bytes;
}

bool decode_manifest(
    std::span<const std::uint8_t> bytes,
    LogicalNodeSourceMapManifest* manifest,
    std::string* error) {
    if (bytes.size() != static_cast<std::size_t>(kManifestSize)) {
        return fail(error, "logical node source-map manifest size mismatch");
    }
    if (!std::equal(kManifestMagic.begin(), kManifestMagic.end(), bytes.begin())) {
        return fail(error, "logical node source-map manifest magic mismatch");
    }
    if (read_u32(bytes, 12U) != 0U || read_u32(bytes, 160U) != 0U ||
        crc32(bytes.first(164U)) != read_u32(bytes, 164U)) {
        return fail(error, "logical node source-map manifest CRC/reserved mismatch");
    }
    if (read_u32(bytes, 152U) != static_cast<std::uint32_t>(kHeadRecordSize) ||
        read_u32(bytes, 156U) != static_cast<std::uint32_t>(kPostingRecordSize)) {
        return fail(error, "logical node source-map record-size contract mismatch");
    }
    manifest->format_version = read_u32(bytes, 8U);
    manifest->source_record_count = read_u64(bytes, 16U);
    manifest->node_count = read_u64(bytes, 24U);
    manifest->posting_count = read_u64(bytes, 32U);
    manifest->candidate_commit.assign(
        reinterpret_cast<const char*>(bytes.data() + 40U), 40U);
    manifest->candidate_tree.assign(
        reinterpret_cast<const char*>(bytes.data() + 80U), 40U);
    std::copy_n(bytes.begin() + 120U, manifest->source_sha256.size(), manifest->source_sha256.begin());
    if (manifest->format_version != kFormatVersion ||
        manifest->source_record_count == 0U || manifest->node_count == 0U ||
        !is_hex_sha(manifest->candidate_commit) || !is_hex_sha(manifest->candidate_tree)) {
        return fail(error, "logical node source-map manifest contract is invalid");
    }
    return true;
}

} // namespace

struct LogicalNodeSourceMapWriter::Impl {
    std::filesystem::path root;
    std::filesystem::path building;
    std::filesystem::path published;
    LogicalNodeSourceMapBuildConfig config;
    std::fstream heads;
    std::fstream postings;
    std::uint64_t posting_count{0U};
    bool begun{false};
    bool finished{false};
    bool poisoned{false};

    Impl(std::filesystem::path store_root, LogicalNodeSourceMapBuildConfig build_config)
        : root(std::move(store_root)),
          building(root / "node-source-map.building"),
          published(root / "node-source-map"),
          config(std::move(build_config)) {}
};

LogicalNodeSourceMapWriter::LogicalNodeSourceMapWriter(
    std::filesystem::path store_root,
    LogicalNodeSourceMapBuildConfig config)
    : impl_(std::make_unique<Impl>(std::move(store_root), std::move(config))) {}

LogicalNodeSourceMapWriter::~LogicalNodeSourceMapWriter() {
    if (impl_ != nullptr && impl_->begun && !impl_->finished) {
        impl_->heads.close();
        impl_->postings.close();
        std::error_code ignored;
        std::filesystem::remove_all(impl_->building, ignored);
    }
}

LogicalNodeSourceMapWriter::LogicalNodeSourceMapWriter(LogicalNodeSourceMapWriter&&) noexcept = default;
LogicalNodeSourceMapWriter& LogicalNodeSourceMapWriter::operator=(LogicalNodeSourceMapWriter&&) noexcept = default;

bool LogicalNodeSourceMapWriter::begin(std::string* error) {
    if (impl_->begun) {
        return fail(error, "logical node source-map writer already began");
    }
    if (!is_hex_sha(impl_->config.candidate_commit) || !is_hex_sha(impl_->config.candidate_tree) ||
        impl_->config.source_record_count == 0U || impl_->config.node_count == 0U) {
        return fail(error, "logical node source-map build config is invalid");
    }
    std::uint64_t heads_size = 0U;
    if (!multiply_size(impl_->config.source_record_count, kHeadRecordSize, &heads_size, error)) {
        return false;
    }
    const auto max_stream = static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max());
    if (heads_size > max_stream) {
        return fail(error, "logical node source-map heads file exceeds stream offset limit");
    }

    std::error_code fs_error;
    if (std::filesystem::exists(impl_->published, fs_error)) {
        if (fs_error) {
            return fail(error, "cannot inspect existing logical node source-map: " + fs_error.message());
        }
        return fail(error, "authoritative logical node source-map already exists");
    }
    fs_error.clear();
    if (std::filesystem::exists(impl_->building, fs_error)) {
        if (fs_error) {
            return fail(error, "cannot inspect stale logical node source-map build: " + fs_error.message());
        }
        std::filesystem::remove_all(impl_->building, fs_error);
        if (fs_error) {
            return fail(error, "cannot clear stale logical node source-map build: " + fs_error.message());
        }
    }
    std::filesystem::create_directories(impl_->building, fs_error);
    if (fs_error) {
        return fail(error, "cannot create logical node source-map build directory: " + fs_error.message());
    }
    impl_->begun = true;

    impl_->heads.open(
        impl_->building / "heads.idx",
        std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    impl_->postings.open(
        impl_->building / "postings.bin",
        std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!impl_->heads || !impl_->postings) {
        impl_->poisoned = true;
        return fail(error, "cannot create logical node source-map files");
    }

    const std::vector<std::uint8_t> empty_head = encode_head(0U);
    std::vector<std::uint8_t> chunk;
    chunk.reserve(kInitChunkRecords * empty_head.size());
    for (std::size_t index = 0U; index < kInitChunkRecords; ++index) {
        chunk.insert(chunk.end(), empty_head.begin(), empty_head.end());
    }
    std::uint64_t remaining = impl_->config.source_record_count;
    while (remaining != 0U) {
        const std::uint64_t batch = std::min<std::uint64_t>(
            remaining, static_cast<std::uint64_t>(kInitChunkRecords));
        const std::size_t bytes = static_cast<std::size_t>(batch) * empty_head.size();
        if (!write_bytes(
                &impl_->heads,
                std::span<const std::uint8_t>(chunk.data(), bytes),
                error)) {
            impl_->poisoned = true;
            return false;
        }
        remaining -= batch;
    }
    impl_->heads.flush();
    if (!impl_->heads) {
        impl_->poisoned = true;
        return fail(error, "logical node source-map head initialization flush failed");
    }
    return true;
}

bool LogicalNodeSourceMapWriter::append_binding(
    std::uint64_t source_record_index,
    std::uint64_t node_ordinal,
    std::string* error) {
    if (!impl_->begun || impl_->finished || impl_->poisoned) {
        return fail(error, "logical node source-map writer is not appendable");
    }
    if (source_record_index >= impl_->config.source_record_count ||
        node_ordinal >= impl_->config.node_count) {
        return fail(error, "logical node source-map binding is out of range");
    }
    if (impl_->posting_count == std::numeric_limits<std::uint64_t>::max()) {
        return fail(error, "logical node source-map posting id space exhausted");
    }

    std::uint64_t head_offset = 0U;
    if (!multiply_size(source_record_index, kHeadRecordSize, &head_offset, error)) {
        return false;
    }
    std::array<std::uint8_t, static_cast<std::size_t>(kHeadRecordSize)> head_bytes{};
    if (!seek_read(&impl_->heads, head_offset, error) ||
        !read_bytes(
            &impl_->heads,
            std::span<std::uint8_t>(head_bytes.data(), head_bytes.size()),
            error)) {
        impl_->poisoned = true;
        return false;
    }
    std::uint64_t previous_head = 0U;
    if (!decode_head(
            std::span<const std::uint8_t>(head_bytes.data(), head_bytes.size()),
            &previous_head,
            error)) {
        impl_->poisoned = true;
        return false;
    }
    if (previous_head > impl_->posting_count) {
        impl_->poisoned = true;
        return fail(error, "logical node source-map head points beyond current postings");
    }

    const Posting posting{source_record_index, node_ordinal, previous_head};
    const std::vector<std::uint8_t> posting_bytes = encode_posting(posting);
    std::uint64_t posting_offset = 0U;
    if (!multiply_size(impl_->posting_count, kPostingRecordSize, &posting_offset, error) ||
        !seek_write(&impl_->postings, posting_offset, error) ||
        !write_bytes(
            &impl_->postings,
            std::span<const std::uint8_t>(posting_bytes.data(), posting_bytes.size()),
            error)) {
        impl_->poisoned = true;
        return false;
    }

    const std::uint64_t new_head = impl_->posting_count + 1U;
    const std::vector<std::uint8_t> new_head_bytes = encode_head(new_head);
    if (!seek_write(&impl_->heads, head_offset, error) ||
        !write_bytes(
            &impl_->heads,
            std::span<const std::uint8_t>(new_head_bytes.data(), new_head_bytes.size()),
            error)) {
        impl_->poisoned = true;
        return false;
    }
    ++impl_->posting_count;
    return true;
}

bool LogicalNodeSourceMapWriter::finish(std::string* error) {
    if (!impl_->begun || impl_->finished || impl_->poisoned) {
        return fail(error, "logical node source-map writer cannot finish");
    }
    impl_->heads.flush();
    impl_->postings.flush();
    if (!impl_->heads || !impl_->postings) {
        impl_->poisoned = true;
        return fail(error, "logical node source-map flush failed");
    }
    impl_->heads.close();
    impl_->postings.close();

    LogicalNodeSourceMapManifest manifest;
    manifest.format_version = kFormatVersion;
    manifest.source_record_count = impl_->config.source_record_count;
    manifest.node_count = impl_->config.node_count;
    manifest.posting_count = impl_->posting_count;
    manifest.candidate_commit = impl_->config.candidate_commit;
    manifest.candidate_tree = impl_->config.candidate_tree;
    manifest.source_sha256 = impl_->config.source_sha256;
    const std::vector<std::uint8_t> bytes = encode_manifest(manifest);
    if (bytes.size() != static_cast<std::size_t>(kManifestSize)) {
        impl_->poisoned = true;
        return fail(error, "logical node source-map manifest encoder produced wrong size");
    }
    std::ofstream manifest_stream(
        impl_->building / "manifest.bin", std::ios::binary | std::ios::trunc);
    if (!manifest_stream ||
        !write_bytes(
            &manifest_stream,
            std::span<const std::uint8_t>(bytes.data(), bytes.size()),
            error)) {
        impl_->poisoned = true;
        return false;
    }
    manifest_stream.flush();
    if (!manifest_stream) {
        impl_->poisoned = true;
        return fail(error, "logical node source-map manifest flush failed");
    }
    manifest_stream.close();

    std::error_code fs_error;
    if (std::filesystem::exists(impl_->published, fs_error)) {
        if (fs_error) {
            impl_->poisoned = true;
            return fail(error, "cannot recheck source-map publication path: " + fs_error.message());
        }
        impl_->poisoned = true;
        return fail(error, "authoritative logical node source-map appeared during build");
    }
    std::filesystem::rename(impl_->building, impl_->published, fs_error);
    if (fs_error) {
        impl_->poisoned = true;
        return fail(error, "cannot publish logical node source-map: " + fs_error.message());
    }
    impl_->finished = true;
    return true;
}

std::uint64_t LogicalNodeSourceMapWriter::posting_count() const noexcept {
    return impl_->posting_count;
}

struct LogicalNodeSourceMapReader::Impl {
    std::filesystem::path root;
    std::filesystem::path published;
    LogicalNodeSourceMapManifest manifest;
    mutable std::ifstream heads;
    mutable std::ifstream postings;
    bool opened{false};

    explicit Impl(std::filesystem::path store_root)
        : root(std::move(store_root)), published(root / "node-source-map") {}

    bool read_posting(
        std::uint64_t posting_plus_one,
        std::uint64_t expected_source_record,
        Posting* posting,
        std::string* error) const {
        if (posting_plus_one == 0U || posting_plus_one > manifest.posting_count) {
            return fail(error, "logical node source-map posting pointer is out of range");
        }
        const std::uint64_t posting_index = posting_plus_one - 1U;
        std::uint64_t offset = 0U;
        if (!multiply_size(posting_index, kPostingRecordSize, &offset, error)) {
            return false;
        }
        std::array<std::uint8_t, static_cast<std::size_t>(kPostingRecordSize)> bytes{};
        if (!seek_read(&postings, offset, error) ||
            !read_bytes(&postings, std::span<std::uint8_t>(bytes.data(), bytes.size()), error) ||
            !decode_posting(
                std::span<const std::uint8_t>(bytes.data(), bytes.size()), posting, error)) {
            return false;
        }
        if (posting->source_record_index != expected_source_record ||
            posting->node_ordinal >= manifest.node_count ||
            posting->next_posting_plus_one > manifest.posting_count) {
            return fail(error, "logical node source-map posting authority mismatch");
        }
        return true;
    }
};

LogicalNodeSourceMapReader::LogicalNodeSourceMapReader(std::filesystem::path store_root)
    : impl_(std::make_unique<Impl>(std::move(store_root))) {}

LogicalNodeSourceMapReader::~LogicalNodeSourceMapReader() = default;
LogicalNodeSourceMapReader::LogicalNodeSourceMapReader(LogicalNodeSourceMapReader&&) noexcept = default;
LogicalNodeSourceMapReader& LogicalNodeSourceMapReader::operator=(LogicalNodeSourceMapReader&&) noexcept = default;

bool LogicalNodeSourceMapReader::open(std::string* error) {
    if (impl_->opened) {
        return fail(error, "logical node source-map reader already opened");
    }
    if (!require_file_size(impl_->published / "manifest.bin", kManifestSize, error)) {
        return false;
    }
    std::ifstream manifest_stream(impl_->published / "manifest.bin", std::ios::binary);
    if (!manifest_stream) {
        return fail(error, "cannot open logical node source-map manifest");
    }
    std::array<std::uint8_t, static_cast<std::size_t>(kManifestSize)> manifest_bytes{};
    if (!read_bytes(
            &manifest_stream,
            std::span<std::uint8_t>(manifest_bytes.data(), manifest_bytes.size()),
            error) ||
        !decode_manifest(
            std::span<const std::uint8_t>(manifest_bytes.data(), manifest_bytes.size()),
            &impl_->manifest,
            error)) {
        return false;
    }
    std::uint64_t expected_heads = 0U;
    std::uint64_t expected_postings = 0U;
    if (!multiply_size(impl_->manifest.source_record_count, kHeadRecordSize, &expected_heads, error) ||
        !multiply_size(impl_->manifest.posting_count, kPostingRecordSize, &expected_postings, error) ||
        !require_file_size(impl_->published / "heads.idx", expected_heads, error) ||
        !require_file_size(impl_->published / "postings.bin", expected_postings, error)) {
        return false;
    }
    impl_->heads.open(impl_->published / "heads.idx", std::ios::binary);
    impl_->postings.open(impl_->published / "postings.bin", std::ios::binary);
    if (!impl_->heads || !impl_->postings) {
        return fail(error, "cannot open logical node source-map data files");
    }
    impl_->opened = true;
    return true;
}

const LogicalNodeSourceMapManifest& LogicalNodeSourceMapReader::manifest() const noexcept {
    return impl_->manifest;
}

bool LogicalNodeSourceMapReader::verify_node_arena_identity(
    const LogicalNodeArenaManifest& arena,
    std::string* error) const {
    if (!impl_->opened) {
        return fail(error, "logical node source-map reader is not open");
    }
    if (arena.candidate_commit != impl_->manifest.candidate_commit ||
        arena.candidate_tree != impl_->manifest.candidate_tree ||
        arena.source_sha256 != impl_->manifest.source_sha256 ||
        arena.node_count != impl_->manifest.node_count) {
        return fail(error, "logical node source-map/node-arena identity mismatch");
    }
    return true;
}

bool LogicalNodeSourceMapReader::first_node_for_record(
    std::uint64_t source_record_index,
    LogicalNodeSourceMapCursor* cursor,
    std::uint64_t* node_ordinal,
    bool* found,
    std::string* error) const {
    if (!impl_->opened || cursor == nullptr || node_ordinal == nullptr || found == nullptr) {
        return fail(error, "logical node source-map first lookup arguments/state are invalid");
    }
    if (source_record_index >= impl_->manifest.source_record_count) {
        return fail(error, "logical node source-map source record is out of range");
    }
    std::uint64_t offset = 0U;
    if (!multiply_size(source_record_index, kHeadRecordSize, &offset, error)) {
        return false;
    }
    std::array<std::uint8_t, static_cast<std::size_t>(kHeadRecordSize)> bytes{};
    if (!seek_read(&impl_->heads, offset, error) ||
        !read_bytes(&impl_->heads, std::span<std::uint8_t>(bytes.data(), bytes.size()), error)) {
        return false;
    }
    std::uint64_t head = 0U;
    if (!decode_head(
            std::span<const std::uint8_t>(bytes.data(), bytes.size()), &head, error)) {
        return false;
    }
    cursor->source_record_index = source_record_index;
    cursor->next_posting_plus_one = 0U;
    cursor->visited_postings = 0U;
    if (head == 0U) {
        *found = false;
        *node_ordinal = 0U;
        return true;
    }
    Posting posting;
    if (!impl_->read_posting(head, source_record_index, &posting, error)) {
        return false;
    }
    cursor->next_posting_plus_one = posting.next_posting_plus_one;
    cursor->visited_postings = 1U;
    *node_ordinal = posting.node_ordinal;
    *found = true;
    return true;
}

bool LogicalNodeSourceMapReader::next_node_for_record(
    LogicalNodeSourceMapCursor* cursor,
    std::uint64_t* node_ordinal,
    bool* found,
    std::string* error) const {
    if (!impl_->opened || cursor == nullptr || node_ordinal == nullptr || found == nullptr ||
        cursor->source_record_index >= impl_->manifest.source_record_count) {
        return fail(error, "logical node source-map cursor arguments/state are invalid");
    }
    if (cursor->next_posting_plus_one == 0U) {
        *found = false;
        *node_ordinal = 0U;
        return true;
    }
    if (cursor->visited_postings >= impl_->manifest.posting_count) {
        return fail(error, "logical node source-map posting chain is cyclic");
    }
    Posting posting;
    if (!impl_->read_posting(
            cursor->next_posting_plus_one,
            cursor->source_record_index,
            &posting,
            error)) {
        return false;
    }
    cursor->next_posting_plus_one = posting.next_posting_plus_one;
    ++cursor->visited_postings;
    *node_ordinal = posting.node_ordinal;
    *found = true;
    return true;
}

} // namespace zevryon::massivedoc
