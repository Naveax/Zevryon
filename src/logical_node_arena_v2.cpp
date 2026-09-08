#include "logical_node_arena_v2.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace zevryon::massivedoc {
namespace {

constexpr std::array<std::uint8_t, 8> kMarkerMagic{
    'Z', 'V', 'N', 'O', 'D', 'V', '0', '2'};
constexpr std::uint32_t kMarkerBytes = 188U;
constexpr std::uint32_t kCrossRecordSpanSemantics = 1U;
constexpr std::uint32_t kExpectedStorageFormatVersion = 1U;

bool fail_v2(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
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

void append_string(std::vector<std::uint8_t>* bytes, std::string_view value) {
    for (const char character : value) {
        bytes->push_back(static_cast<std::uint8_t>(
            static_cast<unsigned char>(character)));
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

std::filesystem::path published_path(const std::filesystem::path& root) {
    return root / "node-arena-v2";
}

std::filesystem::path staging_path(const std::filesystem::path& root) {
    return root / "node-arena-v2.building";
}

std::filesystem::path marker_path(const std::filesystem::path& wrapper_root) {
    return wrapper_root / "manifest-v2.bin";
}

void remove_tree_noexcept(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
}

std::vector<std::uint8_t> encode_marker(
    const LogicalNodeArenaManifest& storage_manifest) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kMarkerBytes);
    bytes.insert(bytes.end(), kMarkerMagic.begin(), kMarkerMagic.end());
    append_u32(&bytes, kLogicalNodeArenaV2FormatVersion);
    append_u32(&bytes, kMarkerBytes);
    append_u32(&bytes, storage_manifest.format_version);
    append_u32(&bytes, kCrossRecordSpanSemantics);
    append_u32(&bytes, storage_manifest.semantic_bucket_count);
    append_u32(&bytes, storage_manifest.semantic_hash_bits);
    append_u64(&bytes, storage_manifest.node_count);
    append_u64(&bytes, storage_manifest.attribute_count);
    for (const std::uint32_t count : storage_manifest.semantic_counts) {
        append_u32(&bytes, count);
    }
    append_u32(&bytes, 0U);
    append_string(&bytes, storage_manifest.candidate_commit);
    append_string(&bytes, storage_manifest.candidate_tree);
    bytes.insert(
        bytes.end(),
        storage_manifest.source_sha256.begin(),
        storage_manifest.source_sha256.end());
    append_u32(
        &bytes,
        crc32(std::span<const std::uint8_t>(bytes.data(), bytes.size())));
    return bytes;
}

bool decode_marker(
    std::span<const std::uint8_t> bytes,
    LogicalNodeArenaV2Manifest* manifest,
    std::string* error) {
    if (manifest == nullptr) {
        return fail_v2(error, "logical node arena v2 manifest output is null");
    }
    if (bytes.size() != kMarkerBytes) {
        return fail_v2(error, "logical node arena v2 marker size mismatch");
    }
    if (!std::equal(kMarkerMagic.begin(), kMarkerMagic.end(), bytes.begin())) {
        return fail_v2(error, "logical node arena v2 marker magic mismatch");
    }
    if (read_u32(bytes, 8U) != kLogicalNodeArenaV2FormatVersion ||
        read_u32(bytes, 12U) != kMarkerBytes) {
        return fail_v2(error, "unsupported logical node arena v2 marker format");
    }
    if (read_u32(bytes, 16U) != kExpectedStorageFormatVersion) {
        return fail_v2(error, "logical node arena v2 storage format is unsupported");
    }
    if (read_u32(bytes, 20U) != kCrossRecordSpanSemantics) {
        return fail_v2(error, "logical node arena v2 source-span semantics are unsupported");
    }
    if (read_u32(bytes, 68U) != 0U) {
        return fail_v2(error, "logical node arena v2 reserved marker bytes are nonzero");
    }
    if (crc32(bytes.first(184U)) != read_u32(bytes, 184U)) {
        return fail_v2(error, "logical node arena v2 marker CRC mismatch");
    }

    LogicalNodeArenaManifest storage;
    storage.format_version = read_u32(bytes, 16U);
    storage.semantic_bucket_count = read_u32(bytes, 24U);
    storage.semantic_hash_bits = read_u32(bytes, 28U);
    storage.node_count = read_u64(bytes, 32U);
    storage.attribute_count = read_u64(bytes, 40U);
    for (std::size_t index = 0U; index < storage.semantic_counts.size(); ++index) {
        storage.semantic_counts[index] = read_u32(bytes, 48U + index * 4U);
    }
    storage.candidate_commit.assign(
        reinterpret_cast<const char*>(bytes.data() + 72U), 40U);
    storage.candidate_tree.assign(
        reinterpret_cast<const char*>(bytes.data() + 112U), 40U);
    std::copy_n(
        bytes.begin() + 152U,
        storage.source_sha256.size(),
        storage.source_sha256.begin());

    manifest->format_version = kLogicalNodeArenaV2FormatVersion;
    manifest->storage_manifest = std::move(storage);
    return true;
}

bool same_storage_manifest(
    const LogicalNodeArenaManifest& marker,
    const LogicalNodeArenaManifest& storage) noexcept {
    return marker.format_version == storage.format_version &&
        marker.semantic_bucket_count == storage.semantic_bucket_count &&
        marker.semantic_hash_bits == storage.semantic_hash_bits &&
        marker.node_count == storage.node_count &&
        marker.attribute_count == storage.attribute_count &&
        marker.semantic_counts == storage.semantic_counts &&
        marker.candidate_commit == storage.candidate_commit &&
        marker.candidate_tree == storage.candidate_tree &&
        marker.source_sha256 == storage.source_sha256;
}

bool write_marker_file(
    const std::filesystem::path& path,
    const LogicalNodeArenaManifest& manifest,
    std::string* error) {
    const std::vector<std::uint8_t> bytes = encode_marker(manifest);
    if (bytes.size() != kMarkerBytes) {
        return fail_v2(error, "logical node arena v2 marker encoding size mismatch");
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return fail_v2(error, "cannot create logical node arena v2 marker");
    }
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream) {
        return fail_v2(error, "logical node arena v2 marker write failed");
    }
    return true;
}

bool read_marker_file(
    const std::filesystem::path& path,
    LogicalNodeArenaV2Manifest* manifest,
    std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return fail_v2(error, "cannot open logical node arena v2 marker");
    }
    std::array<std::uint8_t, kMarkerBytes> bytes{};
    stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        return fail_v2(error, "logical node arena v2 marker is truncated");
    }
    if (stream.peek() != std::char_traits<char>::eof()) {
        return fail_v2(error, "logical node arena v2 marker has trailing bytes");
    }
    return decode_marker(bytes, manifest, error);
}

} // namespace

struct LogicalNodeArenaV2Writer::Impl {
    std::filesystem::path store_root;
    std::filesystem::path published;
    std::filesystem::path staging;
    LogicalNodeArenaBuildConfig config;
    std::unique_ptr<LogicalNodeArenaWriter> storage_writer;
    bool begun{false};
    bool finished{false};

    Impl(std::filesystem::path root, LogicalNodeArenaBuildConfig build_config)
        : store_root(std::move(root)),
          published(published_path(store_root)),
          staging(staging_path(store_root)),
          config(std::move(build_config)) {}
};

LogicalNodeArenaV2Writer::LogicalNodeArenaV2Writer(
    std::filesystem::path store_root,
    LogicalNodeArenaBuildConfig config)
    : impl_(std::make_unique<Impl>(std::move(store_root), std::move(config))) {}

LogicalNodeArenaV2Writer::~LogicalNodeArenaV2Writer() {
    if (impl_ != nullptr && !impl_->finished) {
        impl_->storage_writer.reset();
        remove_tree_noexcept(impl_->staging);
    }
}

LogicalNodeArenaV2Writer::LogicalNodeArenaV2Writer(
    LogicalNodeArenaV2Writer&&) noexcept = default;
LogicalNodeArenaV2Writer& LogicalNodeArenaV2Writer::operator=(
    LogicalNodeArenaV2Writer&&) noexcept = default;

bool LogicalNodeArenaV2Writer::begin(std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (impl_->begun) {
        return fail_v2(error, "logical node arena v2 writer already began");
    }
    if (impl_->store_root.empty()) {
        return fail_v2(error, "logical node arena v2 store root is empty");
    }

    std::error_code fs_error;
    if (std::filesystem::exists(impl_->published, fs_error)) {
        if (fs_error) {
            return fail_v2(
                error,
                "cannot inspect logical node arena v2 output: " + fs_error.message());
        }
        return fail_v2(error, "logical node arena v2 output already exists");
    }
    if (fs_error) {
        return fail_v2(
            error,
            "cannot inspect logical node arena v2 output: " + fs_error.message());
    }

    std::filesystem::remove_all(impl_->staging, fs_error);
    if (fs_error) {
        return fail_v2(
            error,
            "cannot clear stale logical node arena v2 staging tree: " +
                fs_error.message());
    }
    std::filesystem::create_directories(impl_->staging, fs_error);
    if (fs_error) {
        return fail_v2(
            error,
            "cannot create logical node arena v2 staging root: " +
                fs_error.message());
    }

    impl_->storage_writer = std::make_unique<LogicalNodeArenaWriter>(
        impl_->staging, impl_->config);
    if (!impl_->storage_writer->begin(error)) {
        impl_->storage_writer.reset();
        remove_tree_noexcept(impl_->staging);
        return false;
    }
    impl_->begun = true;
    return true;
}

bool LogicalNodeArenaV2Writer::append_node(
    const LogicalNodeInput& node,
    std::span<const LogicalNodeAttributeInput> attributes,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    if (!impl_->begun || impl_->finished || impl_->storage_writer == nullptr) {
        return fail_v2(error, "logical node arena v2 writer is not appendable");
    }
    return impl_->storage_writer->append_node(node, attributes, error);
}

bool LogicalNodeArenaV2Writer::finish(std::string* error) {
    if (error == nullptr) {
        return false;
    }
    if (!impl_->begun || impl_->finished || impl_->storage_writer == nullptr) {
        return fail_v2(error, "logical node arena v2 writer cannot finish");
    }
    if (!impl_->storage_writer->finish(error)) {
        return false;
    }

    LogicalNodeArenaManifest storage_manifest;
    {
        LogicalNodeArenaReader storage_reader(impl_->staging);
        if (!storage_reader.open(error)) {
            return false;
        }
        if (storage_reader.manifest().format_version !=
            kExpectedStorageFormatVersion) {
            return fail_v2(
                error,
                "logical node arena v2 staging storage version is unsupported");
        }
        storage_manifest = storage_reader.manifest();
    }
    if (!write_marker_file(
            marker_path(impl_->staging), storage_manifest, error)) {
        return false;
    }

    std::error_code fs_error;
    const bool output_exists = std::filesystem::exists(impl_->published, fs_error);
    if (fs_error) {
        return fail_v2(
            error,
            "cannot recheck logical node arena v2 output: " + fs_error.message());
    }
    if (output_exists) {
        return fail_v2(error, "logical node arena v2 output appeared during build");
    }

    std::filesystem::rename(impl_->staging, impl_->published, fs_error);
    if (fs_error) {
        return fail_v2(
            error,
            "cannot publish logical node arena v2 atomically: " + fs_error.message());
    }
    impl_->finished = true;
    return true;
}

std::uint64_t LogicalNodeArenaV2Writer::node_count() const noexcept {
    return impl_->storage_writer != nullptr ? impl_->storage_writer->node_count() : 0U;
}

std::uint64_t LogicalNodeArenaV2Writer::attribute_count() const noexcept {
    return impl_->storage_writer != nullptr ?
        impl_->storage_writer->attribute_count() : 0U;
}

std::uint64_t LogicalNodeArenaV2Writer::semantic_bucket_head_bytes() const noexcept {
    return impl_->storage_writer != nullptr ?
        impl_->storage_writer->semantic_bucket_head_bytes() : 0U;
}

struct LogicalNodeArenaV2Reader::Impl {
    std::filesystem::path wrapper_root;
    LogicalNodeArenaV2Manifest manifest{};
    std::unique_ptr<LogicalNodeArenaReader> storage_reader;
    bool opened{false};

    explicit Impl(std::filesystem::path store_root)
        : wrapper_root(published_path(store_root)) {}
};

LogicalNodeArenaV2Reader::LogicalNodeArenaV2Reader(
    std::filesystem::path store_root)
    : impl_(std::make_unique<Impl>(std::move(store_root))) {}

LogicalNodeArenaV2Reader::~LogicalNodeArenaV2Reader() = default;
LogicalNodeArenaV2Reader::LogicalNodeArenaV2Reader(
    LogicalNodeArenaV2Reader&&) noexcept = default;
LogicalNodeArenaV2Reader& LogicalNodeArenaV2Reader::operator=(
    LogicalNodeArenaV2Reader&&) noexcept = default;

bool LogicalNodeArenaV2Reader::open(std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (impl_->opened) {
        return fail_v2(error, "logical node arena v2 reader already opened");
    }

    LogicalNodeArenaV2Manifest marker;
    if (!read_marker_file(marker_path(impl_->wrapper_root), &marker, error)) {
        return false;
    }

    impl_->storage_reader = std::make_unique<LogicalNodeArenaReader>(
        impl_->wrapper_root);
    if (!impl_->storage_reader->open(error)) {
        impl_->storage_reader.reset();
        return false;
    }
    if (!same_storage_manifest(
            marker.storage_manifest,
            impl_->storage_reader->manifest())) {
        impl_->storage_reader.reset();
        return fail_v2(
            error,
            "logical node arena v2 marker does not bind the nested storage manifest");
    }

    impl_->manifest = std::move(marker);
    impl_->opened = true;
    return true;
}

const LogicalNodeArenaV2Manifest& LogicalNodeArenaV2Reader::manifest() const noexcept {
    return impl_->manifest;
}

bool LogicalNodeArenaV2Reader::node_by_ordinal(
    std::uint64_t ordinal,
    LogicalNodeRecord* node,
    std::string* error) const {
    if (!impl_->opened || impl_->storage_reader == nullptr) {
        return fail_v2(error, "logical node arena v2 reader is not open");
    }
    return impl_->storage_reader->node_by_ordinal(ordinal, node, error);
}

bool LogicalNodeArenaV2Reader::node_by_id(
    std::uint64_t logical_id,
    LogicalNodeRecord* node,
    std::string* error) const {
    if (!impl_->opened || impl_->storage_reader == nullptr) {
        return fail_v2(error, "logical node arena v2 reader is not open");
    }
    return impl_->storage_reader->node_by_id(logical_id, node, error);
}

bool LogicalNodeArenaV2Reader::attribute_by_ordinal(
    std::uint64_t ordinal,
    LogicalNodeAttributeRecord* attribute,
    std::string* error) const {
    if (!impl_->opened || impl_->storage_reader == nullptr) {
        return fail_v2(error, "logical node arena v2 reader is not open");
    }
    return impl_->storage_reader->attribute_by_ordinal(ordinal, attribute, error);
}

bool LogicalNodeArenaV2Reader::resolve_semantic(
    LogicalSemanticKind kind,
    std::uint32_t id,
    std::string* value,
    std::string* error) const {
    if (!impl_->opened || impl_->storage_reader == nullptr) {
        return fail_v2(error, "logical node arena v2 reader is not open");
    }
    return impl_->storage_reader->resolve_semantic(kind, id, value, error);
}

} // namespace zevryon::massivedoc
