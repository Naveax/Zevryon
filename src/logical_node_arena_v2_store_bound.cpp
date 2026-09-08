#include "logical_node_arena_v2_store_bound.hpp"

#include "font_content_identity.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zevryon::massivedoc {
namespace {

constexpr std::array<std::uint8_t, 8> kBindingMagic{
    'Z', 'V', 'N', 'S', 'B', '0', '0', '2'};
constexpr std::uint32_t kBindingFormatVersion = 2U;
constexpr std::uint32_t kBindingBytes = 92U;
constexpr std::string_view kCompositeDomain =
    "ZEVRYON-ZVNODA02-SOURCE-BINDING";

bool fail_bound(std::string* error, std::string message) {
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

std::span<const std::byte> as_bytes(
    const std::array<std::uint8_t, 32>& value) noexcept {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(value.data()), value.size());
}

bool compute_composite_source_sha256(
    const LogicalNodeSourceStoreBinding& binding,
    std::array<std::uint8_t, 32>* output,
    std::string* error) {
    if (output == nullptr) {
        return fail_bound(error, "logical node arena v2 composite output is null");
    }

    zevryon::text::Sha256 hasher;
    const auto domain_bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(kCompositeDomain.data()),
        kCompositeDomain.size());
    std::array<std::byte, 8> record_count_bytes{};
    for (unsigned index = 0U; index < 8U; ++index) {
        record_count_bytes[index] = static_cast<std::byte>(
            (binding.source_record_count >> (index * 8U)) & 0xffU);
    }

    if (!hasher.update(domain_bytes) ||
        !hasher.update(as_bytes(binding.payload_sha256)) ||
        !hasher.update(as_bytes(binding.record_sequence_sha256)) ||
        !hasher.update(record_count_bytes)) {
        return fail_bound(error, "logical node arena v2 composite SHA-256 update failed");
    }

    zevryon::text::Sha256Digest digest{};
    if (!hasher.finish(&digest)) {
        return fail_bound(error, "logical node arena v2 composite SHA-256 finish failed");
    }
    for (std::size_t index = 0U; index < output->size(); ++index) {
        (*output)[index] = std::to_integer<std::uint8_t>(digest[index]);
    }
    return true;
}

bool same_binding(
    const LogicalNodeSourceStoreBinding& left,
    const LogicalNodeSourceStoreBinding& right) noexcept {
    return left.source_record_count == right.source_record_count &&
        left.payload_sha256 == right.payload_sha256 &&
        left.record_sequence_sha256 == right.record_sequence_sha256;
}

std::filesystem::path staging_binding_path(const std::filesystem::path& root) {
    return root / "node-arena-v2.building" / "source-binding-v2.bin";
}

std::filesystem::path published_binding_path(const std::filesystem::path& root) {
    return root / "node-arena-v2" / "source-binding-v2.bin";
}

std::vector<std::uint8_t> encode_binding(
    const LogicalNodeSourceStoreBinding& binding) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kBindingBytes);
    bytes.insert(bytes.end(), kBindingMagic.begin(), kBindingMagic.end());
    append_u32(&bytes, kBindingFormatVersion);
    append_u32(&bytes, kBindingBytes);
    bytes.insert(
        bytes.end(), binding.payload_sha256.begin(), binding.payload_sha256.end());
    bytes.insert(
        bytes.end(),
        binding.record_sequence_sha256.begin(),
        binding.record_sequence_sha256.end());
    append_u64(&bytes, binding.source_record_count);
    append_u32(&bytes, crc32(std::span<const std::uint8_t>(bytes.data(), bytes.size())));
    return bytes;
}

bool decode_binding(
    std::span<const std::uint8_t> bytes,
    LogicalNodeSourceStoreBinding* binding,
    std::string* error) {
    if (binding == nullptr) {
        return fail_bound(error, "logical node arena v2 source binding output is null");
    }
    if (bytes.size() != kBindingBytes) {
        return fail_bound(error, "logical node arena v2 source binding size mismatch");
    }
    if (!std::equal(kBindingMagic.begin(), kBindingMagic.end(), bytes.begin())) {
        return fail_bound(error, "logical node arena v2 source binding magic mismatch");
    }
    if (read_u32(bytes, 8U) != kBindingFormatVersion ||
        read_u32(bytes, 12U) != kBindingBytes) {
        return fail_bound(error, "unsupported logical node arena v2 source binding format");
    }
    if (crc32(bytes.first(88U)) != read_u32(bytes, 88U)) {
        return fail_bound(error, "logical node arena v2 source binding CRC mismatch");
    }

    LogicalNodeSourceStoreBinding decoded;
    std::copy_n(bytes.begin() + 16U, decoded.payload_sha256.size(), decoded.payload_sha256.begin());
    std::copy_n(
        bytes.begin() + 48U,
        decoded.record_sequence_sha256.size(),
        decoded.record_sequence_sha256.begin());
    decoded.source_record_count = read_u64(bytes, 80U);
    if (decoded.source_record_count == 0U) {
        return fail_bound(error, "logical node arena v2 source binding has zero records");
    }
    *binding = decoded;
    return true;
}

bool write_binding_file(
    const std::filesystem::path& path,
    const LogicalNodeSourceStoreBinding& binding,
    std::string* error) {
    const std::vector<std::uint8_t> bytes = encode_binding(binding);
    if (bytes.size() != kBindingBytes) {
        return fail_bound(error, "logical node arena v2 source binding encoding size mismatch");
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return fail_bound(error, "cannot create logical node arena v2 source binding");
    }
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream) {
        return fail_bound(error, "logical node arena v2 source binding write failed");
    }
    return true;
}

bool read_binding_file(
    const std::filesystem::path& path,
    LogicalNodeSourceStoreBinding* binding,
    std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return fail_bound(error, "cannot open logical node arena v2 source binding");
    }
    std::array<std::uint8_t, kBindingBytes> bytes{};
    stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        return fail_bound(error, "logical node arena v2 source binding is truncated");
    }
    if (stream.peek() != std::char_traits<char>::eof()) {
        return fail_bound(error, "logical node arena v2 source binding has trailing bytes");
    }
    return decode_binding(bytes, binding, error);
}

} // namespace

struct LogicalNodeArenaV2StoreBoundWriter::Impl {
    std::filesystem::path store_root;
    LogicalNodeArenaBuildConfig config;
    std::unique_ptr<LogicalNodeArenaV2Writer> writer;
    LogicalNodeSourceStoreBinding source_binding{};
    bool begun{false};

    Impl(std::filesystem::path root, LogicalNodeArenaBuildConfig build_config)
        : store_root(std::move(root)), config(std::move(build_config)) {}
};

LogicalNodeArenaV2StoreBoundWriter::LogicalNodeArenaV2StoreBoundWriter(
    std::filesystem::path store_root,
    LogicalNodeArenaBuildConfig config)
    : impl_(std::make_unique<Impl>(std::move(store_root), std::move(config))) {}

LogicalNodeArenaV2StoreBoundWriter::~LogicalNodeArenaV2StoreBoundWriter() = default;
LogicalNodeArenaV2StoreBoundWriter::LogicalNodeArenaV2StoreBoundWriter(
    LogicalNodeArenaV2StoreBoundWriter&&) noexcept = default;
LogicalNodeArenaV2StoreBoundWriter& LogicalNodeArenaV2StoreBoundWriter::operator=(
    LogicalNodeArenaV2StoreBoundWriter&&) noexcept = default;

bool LogicalNodeArenaV2StoreBoundWriter::begin(std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (impl_->begun) {
        return fail_bound(error, "logical node arena v2 store-bound writer already began");
    }

    LogicalNodeSourceStoreBinding actual;
    if (!inspect_logical_node_source_store_binding(impl_->store_root, &actual, error)) {
        return false;
    }
    if (actual.source_record_count == 0U) {
        return fail_bound(error, "logical node arena v2 requires a non-empty native store");
    }
    if (impl_->config.source_sha256 != actual.payload_sha256) {
        return fail_bound(error, "logical node arena v2 build config payload SHA-256 does not match store");
    }

    std::array<std::uint8_t, 32> composite{};
    if (!compute_composite_source_sha256(actual, &composite, error)) {
        return false;
    }
    LogicalNodeArenaBuildConfig storage_config = impl_->config;
    storage_config.source_sha256 = composite;

    impl_->writer.reset(
        new LogicalNodeArenaV2Writer(impl_->store_root, std::move(storage_config)));
    if (!impl_->writer->begin(error)) {
        impl_->writer.reset();
        return false;
    }
    if (!write_binding_file(staging_binding_path(impl_->store_root), actual, error)) {
        impl_->writer.reset();
        return false;
    }
    impl_->source_binding = actual;
    impl_->begun = true;
    return true;
}

bool LogicalNodeArenaV2StoreBoundWriter::append_node(
    const LogicalNodeInput& node,
    std::span<const LogicalNodeAttributeInput> attributes,
    std::string* error) {
    if (error == nullptr || !impl_->begun || impl_->writer == nullptr) {
        if (error != nullptr) {
            *error = "logical node arena v2 store-bound writer is not appendable";
        }
        return false;
    }
    return impl_->writer->append_node(node, attributes, error);
}

bool LogicalNodeArenaV2StoreBoundWriter::finish(std::string* error) {
    if (error == nullptr || !impl_->begun || impl_->writer == nullptr) {
        if (error != nullptr) {
            *error = "logical node arena v2 store-bound writer cannot finish";
        }
        return false;
    }
    return impl_->writer->finish(error);
}

std::uint64_t LogicalNodeArenaV2StoreBoundWriter::node_count() const noexcept {
    return impl_->writer != nullptr ? impl_->writer->node_count() : 0U;
}

std::uint64_t LogicalNodeArenaV2StoreBoundWriter::attribute_count() const noexcept {
    return impl_->writer != nullptr ? impl_->writer->attribute_count() : 0U;
}

std::uint64_t LogicalNodeArenaV2StoreBoundWriter::semantic_bucket_head_bytes() const noexcept {
    return impl_->writer != nullptr ? impl_->writer->semantic_bucket_head_bytes() : 0U;
}

const LogicalNodeSourceStoreBinding&
LogicalNodeArenaV2StoreBoundWriter::source_binding() const noexcept {
    return impl_->source_binding;
}

struct LogicalNodeArenaV2StoreBoundReader::Impl {
    std::filesystem::path store_root;
    std::unique_ptr<LogicalNodeArenaV2Reader> reader;
    LogicalNodeSourceStoreBinding source_binding{};
    bool opened{false};

    explicit Impl(std::filesystem::path root) : store_root(std::move(root)) {}
};

LogicalNodeArenaV2StoreBoundReader::LogicalNodeArenaV2StoreBoundReader(
    std::filesystem::path store_root)
    : impl_(std::make_unique<Impl>(std::move(store_root))) {}

LogicalNodeArenaV2StoreBoundReader::~LogicalNodeArenaV2StoreBoundReader() = default;
LogicalNodeArenaV2StoreBoundReader::LogicalNodeArenaV2StoreBoundReader(
    LogicalNodeArenaV2StoreBoundReader&&) noexcept = default;
LogicalNodeArenaV2StoreBoundReader& LogicalNodeArenaV2StoreBoundReader::operator=(
    LogicalNodeArenaV2StoreBoundReader&&) noexcept = default;

bool LogicalNodeArenaV2StoreBoundReader::open(std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (impl_->opened) {
        return fail_bound(error, "logical node arena v2 store-bound reader is already open");
    }

    LogicalNodeSourceStoreBinding encoded;
    if (!read_binding_file(published_binding_path(impl_->store_root), &encoded, error)) {
        return false;
    }
    LogicalNodeSourceStoreBinding actual;
    if (!inspect_logical_node_source_store_binding(impl_->store_root, &actual, error)) {
        return false;
    }
    if (!same_binding(encoded, actual)) {
        return fail_bound(
            error,
            "logical node arena v2 source binding does not match authoritative store");
    }

    std::array<std::uint8_t, 32> composite{};
    if (!compute_composite_source_sha256(encoded, &composite, error)) {
        return false;
    }

    impl_->reader.reset(new LogicalNodeArenaV2Reader(impl_->store_root));
    if (!impl_->reader->open(error)) {
        impl_->reader.reset();
        return false;
    }
    if (impl_->reader->manifest().storage_manifest.source_sha256 != composite) {
        impl_->reader.reset();
        return fail_bound(
            error,
            "logical node arena v2 source binding does not bind nested arena");
    }

    impl_->source_binding = encoded;
    impl_->opened = true;
    return true;
}

const LogicalNodeArenaV2Manifest&
LogicalNodeArenaV2StoreBoundReader::manifest() const noexcept {
    static const LogicalNodeArenaV2Manifest empty{};
    return impl_->reader != nullptr ? impl_->reader->manifest() : empty;
}

const LogicalNodeSourceStoreBinding&
LogicalNodeArenaV2StoreBoundReader::source_binding() const noexcept {
    return impl_->source_binding;
}

bool LogicalNodeArenaV2StoreBoundReader::node_by_ordinal(
    std::uint64_t ordinal,
    LogicalNodeRecord* node,
    std::string* error) const {
    if (!impl_->opened || impl_->reader == nullptr) {
        return fail_bound(error, "logical node arena v2 store-bound reader is not open");
    }
    return impl_->reader->node_by_ordinal(ordinal, node, error);
}

bool LogicalNodeArenaV2StoreBoundReader::node_by_id(
    std::uint64_t logical_id,
    LogicalNodeRecord* node,
    std::string* error) const {
    if (!impl_->opened || impl_->reader == nullptr) {
        return fail_bound(error, "logical node arena v2 store-bound reader is not open");
    }
    return impl_->reader->node_by_id(logical_id, node, error);
}

bool LogicalNodeArenaV2StoreBoundReader::attribute_by_ordinal(
    std::uint64_t ordinal,
    LogicalNodeAttributeRecord* attribute,
    std::string* error) const {
    if (!impl_->opened || impl_->reader == nullptr) {
        return fail_bound(error, "logical node arena v2 store-bound reader is not open");
    }
    return impl_->reader->attribute_by_ordinal(ordinal, attribute, error);
}

bool LogicalNodeArenaV2StoreBoundReader::resolve_semantic(
    LogicalSemanticKind kind,
    std::uint32_t id,
    std::string* value,
    std::string* error) const {
    if (!impl_->opened || impl_->reader == nullptr) {
        return fail_bound(error, "logical node arena v2 store-bound reader is not open");
    }
    return impl_->reader->resolve_semantic(kind, id, value, error);
}

} // namespace zevryon::massivedoc
