#include "logical_node_source_v2.hpp"

#include "massivedoc_store.hpp"

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

constexpr std::array<std::uint8_t, 8> kSourceMagic{
    'Z', 'V', 'N', 'S', 'R', 'C', '0', '1'};
constexpr std::uint32_t kSourceHeaderBytes = 112U;
constexpr std::uint32_t kNodeFrameFixedBytes = 64U;
constexpr std::uint32_t kNodeFrameMinimumBytes = kNodeFrameFixedBytes + 4U;
constexpr std::uint32_t kMaximumSemanticBytes = 1024U * 1024U;
constexpr std::uint32_t kMaximumNodeFrameBytes = 16U * 1024U * 1024U;
constexpr std::uint32_t kMaximumAttributesPerNode = 65536U;
constexpr std::size_t kMinimumEncodedAttributeBytes = 17U;

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

void store_u32(
    std::vector<std::uint8_t>* bytes,
    std::size_t offset,
    std::uint32_t value) {
    for (unsigned index = 0U; index < 4U; ++index) {
        (*bytes)[offset + index] = static_cast<std::uint8_t>(
            (value >> (index * 8U)) & 0xffU);
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
    bytes->reserve(bytes->size() + value.size());
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

bool stream_count(std::size_t size, std::streamsize* output, std::string* error) {
    const auto maximum = static_cast<std::uintmax_t>(
        std::numeric_limits<std::streamsize>::max());
    if (static_cast<std::uintmax_t>(size) > maximum) {
        return fail_v2(error, "logical node source v2 I/O size exceeds stream limit");
    }
    *output = static_cast<std::streamsize>(size);
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
        return fail_v2(error, "logical node source v2 write failed");
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
        return fail_v2(error, "logical node source v2 is truncated");
    }
    return true;
}

std::vector<std::uint8_t> encode_header(const LogicalNodeSourceManifest& manifest) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kSourceHeaderBytes);
    bytes.insert(bytes.end(), kSourceMagic.begin(), kSourceMagic.end());
    append_u32(&bytes, manifest.format_version);
    append_u32(&bytes, kSourceHeaderBytes);
    append_u64(&bytes, manifest.node_count);
    append_u64(&bytes, manifest.attribute_count);
    append_u64(&bytes, manifest.store_binding.source_record_count);
    bytes.insert(
        bytes.end(),
        manifest.store_binding.payload_sha256.begin(),
        manifest.store_binding.payload_sha256.end());
    bytes.insert(
        bytes.end(),
        manifest.store_binding.record_sequence_sha256.begin(),
        manifest.store_binding.record_sequence_sha256.end());
    append_u32(&bytes, 0U);
    append_u32(
        &bytes,
        crc32(std::span<const std::uint8_t>(bytes.data(), bytes.size())));
    return bytes;
}

bool decode_header(
    std::span<const std::uint8_t> bytes,
    LogicalNodeSourceManifest* manifest,
    std::string* error) {
    if (manifest == nullptr) {
        return fail_v2(error, "logical node source v2 manifest output is null");
    }
    if (bytes.size() != kSourceHeaderBytes) {
        return fail_v2(error, "logical node source v2 header size mismatch");
    }
    if (!std::equal(kSourceMagic.begin(), kSourceMagic.end(), bytes.begin())) {
        return fail_v2(error, "logical node source v2 magic mismatch");
    }
    if (read_u32(bytes, 8U) != kLogicalNodeSourceV2FormatVersion ||
        read_u32(bytes, 12U) != kSourceHeaderBytes) {
        return fail_v2(error, "unsupported logical node source v2 format");
    }
    if (read_u32(bytes, 104U) != 0U) {
        return fail_v2(error, "logical node source v2 reserved header bytes are nonzero");
    }
    if (crc32(bytes.first(108U)) != read_u32(bytes, 108U)) {
        return fail_v2(error, "logical node source v2 header CRC mismatch");
    }

    manifest->format_version = read_u32(bytes, 8U);
    manifest->node_count = read_u64(bytes, 16U);
    manifest->attribute_count = read_u64(bytes, 24U);
    manifest->store_binding.source_record_count = read_u64(bytes, 32U);
    std::copy_n(
        bytes.begin() + 40U,
        manifest->store_binding.payload_sha256.size(),
        manifest->store_binding.payload_sha256.begin());
    std::copy_n(
        bytes.begin() + 72U,
        manifest->store_binding.record_sequence_sha256.size(),
        manifest->store_binding.record_sequence_sha256.begin());
    return true;
}

bool same_binding(
    const LogicalNodeSourceStoreBinding& left,
    const LogicalNodeSourceStoreBinding& right) noexcept {
    return left.source_record_count == right.source_record_count &&
        left.payload_sha256 == right.payload_sha256 &&
        left.record_sequence_sha256 == right.record_sequence_sha256;
}

bool valid_semantic(std::string_view value) noexcept {
    return value.size() <= kMaximumSemanticBytes &&
        value.size() <= static_cast<std::size_t>(
            std::numeric_limits<std::uint32_t>::max());
}

bool bounded_add(std::uint64_t* total, std::uint64_t amount) noexcept {
    if (*total > std::numeric_limits<std::uint64_t>::max() - amount) {
        return false;
    }
    *total += amount;
    return true;
}

std::filesystem::path building_path(const std::filesystem::path& output) {
    std::filesystem::path result = output;
    result += ".building";
    return result;
}

void remove_file_noexcept(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

} // namespace

struct LogicalNodeSourceV2Writer::Impl {
    std::filesystem::path output;
    std::filesystem::path building;
    std::fstream stream;
    std::uint64_t node_count{0U};
    std::uint64_t attribute_count{0U};
    std::uint64_t maximum_source_record_index{0U};
    bool have_source_record{false};
    bool begun{false};
    bool finished{false};
    bool poisoned{false};

    explicit Impl(std::filesystem::path output_path)
        : output(std::move(output_path)), building(building_path(output)) {}
};

LogicalNodeSourceV2Writer::LogicalNodeSourceV2Writer(
    std::filesystem::path output_path)
    : impl_(std::make_unique<Impl>(std::move(output_path))) {}

LogicalNodeSourceV2Writer::~LogicalNodeSourceV2Writer() {
    if (impl_ != nullptr && !impl_->finished) {
        impl_->stream.close();
        remove_file_noexcept(impl_->building);
    }
}

LogicalNodeSourceV2Writer::LogicalNodeSourceV2Writer(
    LogicalNodeSourceV2Writer&&) noexcept = default;
LogicalNodeSourceV2Writer& LogicalNodeSourceV2Writer::operator=(
    LogicalNodeSourceV2Writer&&) noexcept = default;

bool LogicalNodeSourceV2Writer::begin(std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (impl_->begun) {
        return fail_v2(error, "logical node source v2 writer already began");
    }
    if (impl_->output.empty()) {
        return fail_v2(error, "logical node source v2 output path is empty");
    }

    std::error_code fs_error;
    const bool output_exists = std::filesystem::exists(impl_->output, fs_error);
    if (fs_error) {
        return fail_v2(
            error,
            "cannot inspect logical node source v2 output: " + fs_error.message());
    }
    if (output_exists) {
        return fail_v2(error, "logical node source v2 output already exists");
    }

    fs_error.clear();
    std::filesystem::remove(impl_->building, fs_error);
    if (fs_error) {
        return fail_v2(
            error,
            "cannot clear stale logical node source v2 build: " + fs_error.message());
    }
    const std::filesystem::path parent = impl_->output.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, fs_error);
        if (fs_error) {
            return fail_v2(
                error,
                "cannot create logical node source v2 parent: " + fs_error.message());
        }
    }

    impl_->stream.open(
        impl_->building,
        std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!impl_->stream) {
        return fail_v2(error, "cannot create logical node source v2 build file");
    }
    impl_->begun = true;

    LogicalNodeSourceManifest placeholder;
    placeholder.format_version = kLogicalNodeSourceV2FormatVersion;
    const std::vector<std::uint8_t> header = encode_header(placeholder);
    if (header.size() != kSourceHeaderBytes ||
        !write_bytes(
            &impl_->stream,
            std::span<const std::uint8_t>(header.data(), header.size()),
            error)) {
        impl_->poisoned = true;
        impl_->stream.close();
        remove_file_noexcept(impl_->building);
        return false;
    }
    return true;
}

bool LogicalNodeSourceV2Writer::append_node(
    const LogicalNodeInput& node,
    std::span<const LogicalNodeAttributeInput> attributes,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    if (!impl_->begun || impl_->finished || impl_->poisoned) {
        return fail_v2(error, "logical node source v2 writer is not appendable");
    }

    const std::uint64_t ordinal = impl_->node_count;
    if (ordinal == std::numeric_limits<std::uint64_t>::max() ||
        node.logical_id != ordinal + 1U) {
        return fail_v2(
            error,
            "logical node source v2 ids must be contiguous 1-based pre-order ordinals");
    }
    if ((ordinal == 0U && node.parent_ordinal != kNoLogicalNodeOrdinal) ||
        (ordinal != 0U &&
         (node.parent_ordinal == kNoLogicalNodeOrdinal ||
          node.parent_ordinal >= ordinal))) {
        return fail_v2(error, "logical node source v2 parent ordering is invalid");
    }
    if (node.tag.empty() || !valid_semantic(node.tag) ||
        !valid_semantic(node.role) || !valid_semantic(node.style)) {
        return fail_v2(error, "logical node source v2 semantic length is invalid");
    }
    if (attributes.size() > kMaximumAttributesPerNode) {
        return fail_v2(error, "logical node source v2 node has too many attributes");
    }
    if (impl_->attribute_count >
        std::numeric_limits<std::uint64_t>::max() - attributes.size()) {
        return fail_v2(error, "logical node source v2 attribute count overflows");
    }

    std::uint64_t encoded_without_crc = kNodeFrameFixedBytes;
    if (!bounded_add(&encoded_without_crc, node.tag.size()) ||
        !bounded_add(&encoded_without_crc, node.role.size()) ||
        !bounded_add(&encoded_without_crc, node.style.size())) {
        return fail_v2(error, "logical node source v2 frame size overflows");
    }
    for (const LogicalNodeAttributeInput& attribute : attributes) {
        if (attribute.name.empty() || !valid_semantic(attribute.name) ||
            !valid_semantic(attribute.value)) {
            return fail_v2(
                error,
                "logical node source v2 attribute semantic length is invalid");
        }
        if (!bounded_add(&encoded_without_crc, 16U) ||
            !bounded_add(&encoded_without_crc, attribute.name.size()) ||
            !bounded_add(&encoded_without_crc, attribute.value.size())) {
            return fail_v2(error, "logical node source v2 frame size overflows");
        }
    }
    if (encoded_without_crc > kMaximumNodeFrameBytes - sizeof(std::uint32_t)) {
        return fail_v2(error, "logical node source v2 frame exceeds bounded size");
    }

    std::vector<std::uint8_t> frame;
    frame.reserve(
        static_cast<std::size_t>(encoded_without_crc) + sizeof(std::uint32_t));
    append_u32(&frame, 0U);
    append_u32(&frame, static_cast<std::uint32_t>(attributes.size()));
    append_u64(&frame, node.logical_id);
    append_u64(&frame, node.source_record_index);
    append_u64(&frame, node.source_byte_offset);
    append_u64(&frame, node.source_byte_length);
    append_u64(&frame, node.parent_ordinal);
    append_u32(&frame, node.flags);
    append_u32(&frame, static_cast<std::uint32_t>(node.tag.size()));
    append_u32(&frame, static_cast<std::uint32_t>(node.role.size()));
    append_u32(&frame, static_cast<std::uint32_t>(node.style.size()));
    append_string(&frame, node.tag);
    append_string(&frame, node.role);
    append_string(&frame, node.style);
    for (const LogicalNodeAttributeInput& attribute : attributes) {
        append_u32(&frame, static_cast<std::uint32_t>(attribute.name.size()));
        append_u32(&frame, static_cast<std::uint32_t>(attribute.value.size()));
        append_u32(&frame, attribute.flags);
        append_u32(&frame, 0U);
        append_string(&frame, attribute.name);
        append_string(&frame, attribute.value);
    }
    if (frame.size() != encoded_without_crc) {
        impl_->poisoned = true;
        return fail_v2(error, "logical node source v2 encoded frame size mismatch");
    }

    const std::uint32_t frame_bytes = static_cast<std::uint32_t>(
        frame.size() + sizeof(std::uint32_t));
    store_u32(&frame, 0U, frame_bytes);
    append_u32(
        &frame,
        crc32(std::span<const std::uint8_t>(frame.data(), frame.size())));
    if (!write_bytes(
            &impl_->stream,
            std::span<const std::uint8_t>(frame.data(), frame.size()),
            error)) {
        impl_->poisoned = true;
        return false;
    }

    ++impl_->node_count;
    impl_->attribute_count += static_cast<std::uint64_t>(attributes.size());
    if (!impl_->have_source_record ||
        node.source_record_index > impl_->maximum_source_record_index) {
        impl_->maximum_source_record_index = node.source_record_index;
        impl_->have_source_record = true;
    }
    return true;
}

bool LogicalNodeSourceV2Writer::finish(
    const LogicalNodeSourceStoreBinding& store_binding,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    if (!impl_->begun || impl_->finished || impl_->poisoned) {
        return fail_v2(error, "logical node source v2 writer cannot finish");
    }
    if (impl_->node_count == 0U || !impl_->have_source_record) {
        return fail_v2(error, "logical node source v2 cannot publish an empty stream");
    }
    if (store_binding.source_record_count == 0U ||
        impl_->maximum_source_record_index >= store_binding.source_record_count) {
        return fail_v2(
            error,
            "logical node source v2 references a record outside its store binding");
    }

    LogicalNodeSourceManifest manifest;
    manifest.format_version = kLogicalNodeSourceV2FormatVersion;
    manifest.node_count = impl_->node_count;
    manifest.attribute_count = impl_->attribute_count;
    manifest.store_binding = store_binding;
    const std::vector<std::uint8_t> header = encode_header(manifest);

    impl_->stream.flush();
    if (!impl_->stream) {
        impl_->poisoned = true;
        return fail_v2(error, "logical node source v2 flush failed");
    }
    impl_->stream.clear();
    impl_->stream.seekp(0, std::ios::beg);
    if (!impl_->stream ||
        !write_bytes(
            &impl_->stream,
            std::span<const std::uint8_t>(header.data(), header.size()),
            error)) {
        impl_->poisoned = true;
        return false;
    }
    impl_->stream.flush();
    if (!impl_->stream) {
        impl_->poisoned = true;
        return fail_v2(error, "logical node source v2 header flush failed");
    }
    impl_->stream.close();

    std::error_code fs_error;
    const bool output_exists = std::filesystem::exists(impl_->output, fs_error);
    if (fs_error) {
        impl_->poisoned = true;
        return fail_v2(
            error,
            "cannot recheck logical node source v2 output: " + fs_error.message());
    }
    if (output_exists) {
        impl_->poisoned = true;
        return fail_v2(error, "logical node source v2 output appeared during build");
    }
    std::filesystem::rename(impl_->building, impl_->output, fs_error);
    if (fs_error) {
        impl_->poisoned = true;
        return fail_v2(
            error,
            "cannot publish logical node source v2: " + fs_error.message());
    }
    impl_->finished = true;
    return true;
}

std::uint64_t LogicalNodeSourceV2Writer::node_count() const noexcept {
    return impl_->node_count;
}

std::uint64_t LogicalNodeSourceV2Writer::attribute_count() const noexcept {
    return impl_->attribute_count;
}

struct LogicalNodeSourceV2Reader::Impl {
    std::filesystem::path source;
    std::ifstream stream;
    LogicalNodeSourceManifest manifest{};
    std::uint64_t consumed_nodes{0U};
    std::uint64_t consumed_attributes{0U};
    bool opened{false};
    bool ended{false};

    explicit Impl(std::filesystem::path source_path)
        : source(std::move(source_path)) {}
};

LogicalNodeSourceV2Reader::LogicalNodeSourceV2Reader(
    std::filesystem::path source_path)
    : impl_(std::make_unique<Impl>(std::move(source_path))) {}

LogicalNodeSourceV2Reader::~LogicalNodeSourceV2Reader() = default;
LogicalNodeSourceV2Reader::LogicalNodeSourceV2Reader(
    LogicalNodeSourceV2Reader&&) noexcept = default;
LogicalNodeSourceV2Reader& LogicalNodeSourceV2Reader::operator=(
    LogicalNodeSourceV2Reader&&) noexcept = default;

bool LogicalNodeSourceV2Reader::open(std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (impl_->opened) {
        return fail_v2(error, "logical node source v2 reader already opened");
    }
    impl_->stream.open(impl_->source, std::ios::binary);
    if (!impl_->stream) {
        return fail_v2(error, "cannot open logical node source v2");
    }

    std::array<std::uint8_t, kSourceHeaderBytes> header{};
    if (!read_bytes(
            &impl_->stream,
            std::span<std::uint8_t>(header.data(), header.size()),
            error) ||
        !decode_header(
            std::span<const std::uint8_t>(header.data(), header.size()),
            &impl_->manifest,
            error)) {
        return false;
    }
    if (impl_->manifest.node_count == 0U) {
        return fail_v2(error, "logical node source v2 manifest declares no nodes");
    }
    if (impl_->manifest.store_binding.source_record_count == 0U) {
        return fail_v2(
            error,
            "logical node source v2 manifest declares no source records");
    }
    impl_->opened = true;
    return true;
}

const LogicalNodeSourceManifest& LogicalNodeSourceV2Reader::manifest() const noexcept {
    return impl_->manifest;
}

bool LogicalNodeSourceV2Reader::next(
    LogicalNodeSourceNode* node,
    bool* has_node,
    std::string* error) {
    if (node == nullptr || has_node == nullptr || error == nullptr) {
        return false;
    }
    *has_node = false;
    if (!impl_->opened || impl_->ended) {
        return fail_v2(error, "logical node source v2 reader is not readable");
    }

    if (impl_->consumed_nodes == impl_->manifest.node_count) {
        if (impl_->consumed_attributes != impl_->manifest.attribute_count) {
            return fail_v2(
                error,
                "logical node source v2 attribute count disagrees with manifest");
        }
        const int trailing = impl_->stream.peek();
        if (trailing != std::char_traits<char>::eof()) {
            return fail_v2(error, "logical node source v2 has trailing bytes");
        }
        impl_->ended = true;
        error->clear();
        return true;
    }

    std::array<std::uint8_t, 4> size_bytes{};
    if (!read_bytes(
            &impl_->stream,
            std::span<std::uint8_t>(size_bytes.data(), size_bytes.size()),
            error)) {
        return false;
    }
    const std::uint32_t frame_bytes = read_u32(
        std::span<const std::uint8_t>(size_bytes.data(), size_bytes.size()), 0U);
    if (frame_bytes < kNodeFrameMinimumBytes ||
        frame_bytes > kMaximumNodeFrameBytes) {
        return fail_v2(
            error,
            "logical node source v2 frame size is outside the bounded contract");
    }

    std::vector<std::uint8_t> frame(frame_bytes);
    std::copy(size_bytes.begin(), size_bytes.end(), frame.begin());
    if (!read_bytes(
            &impl_->stream,
            std::span<std::uint8_t>(
                frame.data() + size_bytes.size(),
                frame.size() - size_bytes.size()),
            error)) {
        return false;
    }
    const std::span<const std::uint8_t> bytes(frame.data(), frame.size());
    const std::size_t crc_offset = frame.size() - sizeof(std::uint32_t);
    if (crc32(bytes.first(crc_offset)) != read_u32(bytes, crc_offset)) {
        return fail_v2(error, "logical node source v2 frame CRC mismatch");
    }

    const std::uint32_t attribute_count = read_u32(bytes, 4U);
    if (attribute_count > kMaximumAttributesPerNode) {
        return fail_v2(
            error,
            "logical node source v2 node exceeds bounded attribute count");
    }

    LogicalNodeSourceNode output;
    output.logical_id = read_u64(bytes, 8U);
    output.source_record_index = read_u64(bytes, 16U);
    output.source_byte_offset = read_u64(bytes, 24U);
    output.source_byte_length = read_u64(bytes, 32U);
    output.parent_ordinal = read_u64(bytes, 40U);
    output.flags = read_u32(bytes, 48U);
    const std::uint32_t tag_bytes = read_u32(bytes, 52U);
    const std::uint32_t role_bytes = read_u32(bytes, 56U);
    const std::uint32_t style_bytes = read_u32(bytes, 60U);

    const std::uint64_t ordinal = impl_->consumed_nodes;
    if (ordinal == std::numeric_limits<std::uint64_t>::max() ||
        output.logical_id != ordinal + 1U) {
        return fail_v2(
            error,
            "logical node source v2 ids violate contiguous pre-order identity");
    }
    if ((ordinal == 0U && output.parent_ordinal != kNoLogicalNodeOrdinal) ||
        (ordinal != 0U &&
         (output.parent_ordinal == kNoLogicalNodeOrdinal ||
          output.parent_ordinal >= ordinal))) {
        return fail_v2(error, "logical node source v2 parent ordering is invalid");
    }
    if (tag_bytes == 0U || tag_bytes > kMaximumSemanticBytes ||
        role_bytes > kMaximumSemanticBytes ||
        style_bytes > kMaximumSemanticBytes) {
        return fail_v2(error, "logical node source v2 semantic length is invalid");
    }
    if (output.source_record_index >=
        impl_->manifest.store_binding.source_record_count) {
        return fail_v2(
            error,
            "logical node source v2 references a source record outside its binding");
    }

    std::size_t cursor = kNodeFrameFixedBytes;
    const auto take_string = [&](std::uint32_t length, std::string* target) -> bool {
        const std::size_t amount = static_cast<std::size_t>(length);
        if (cursor > crc_offset || amount > crc_offset - cursor) {
            return false;
        }
        target->assign(
            reinterpret_cast<const char*>(frame.data() + cursor), amount);
        cursor += amount;
        return true;
    };

    if (!take_string(tag_bytes, &output.tag) ||
        !take_string(role_bytes, &output.role) ||
        !take_string(style_bytes, &output.style)) {
        return fail_v2(error, "logical node source v2 semantic payload escapes frame");
    }

    const std::size_t remaining_after_semantics =
        cursor <= crc_offset ? crc_offset - cursor : 0U;
    if (attribute_count != 0U &&
        static_cast<std::size_t>(attribute_count) >
            remaining_after_semantics / kMinimumEncodedAttributeBytes) {
        return fail_v2(
            error,
            "logical node source v2 attribute count cannot fit inside frame");
    }
    output.attributes.reserve(attribute_count);
    for (std::uint32_t index = 0U; index < attribute_count; ++index) {
        if (cursor > crc_offset || crc_offset - cursor < 16U) {
            return fail_v2(error, "logical node source v2 attribute header escapes frame");
        }
        const std::uint32_t name_bytes = read_u32(bytes, cursor);
        const std::uint32_t value_bytes = read_u32(bytes, cursor + 4U);
        const std::uint32_t flags = read_u32(bytes, cursor + 8U);
        const std::uint32_t reserved = read_u32(bytes, cursor + 12U);
        cursor += 16U;
        if (name_bytes == 0U || name_bytes > kMaximumSemanticBytes ||
            value_bytes > kMaximumSemanticBytes || reserved != 0U) {
            return fail_v2(
                error,
                "logical node source v2 attribute header is invalid");
        }
        LogicalNodeSourceAttribute attribute;
        attribute.flags = flags;
        if (!take_string(name_bytes, &attribute.name) ||
            !take_string(value_bytes, &attribute.value)) {
            return fail_v2(
                error,
                "logical node source v2 attribute payload escapes frame");
        }
        output.attributes.push_back(std::move(attribute));
    }
    if (cursor != crc_offset) {
        return fail_v2(error, "logical node source v2 frame has unclaimed payload bytes");
    }

    if (impl_->consumed_attributes >
        std::numeric_limits<std::uint64_t>::max() - attribute_count) {
        return fail_v2(error, "logical node source v2 attribute counter overflows");
    }
    impl_->consumed_attributes += attribute_count;
    if (impl_->consumed_attributes > impl_->manifest.attribute_count) {
        return fail_v2(
            error,
            "logical node source v2 attributes exceed manifest count");
    }
    ++impl_->consumed_nodes;
    *node = std::move(output);
    *has_node = true;
    return true;
}

bool validate_logical_node_source_v2_against_store(
    const std::filesystem::path& source_path,
    const std::filesystem::path& store_root,
    LogicalNodeSourceV2ValidationStats* stats,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();

    LogicalNodeSourceStoreBinding actual_binding;
    if (!inspect_logical_node_source_store_binding(
            store_root, &actual_binding, error)) {
        return false;
    }

    LogicalNodeSourceV2Reader source(source_path);
    if (!source.open(error)) {
        return false;
    }
    if (!same_binding(source.manifest().store_binding, actual_binding)) {
        return fail_v2(
            error,
            "logical node source v2 binding does not match authoritative store");
    }

    StoreReader store(store_root);
    if (!store.open(error)) {
        return false;
    }
    if (store.stats().corpus.logical_records != actual_binding.source_record_count ||
        source.manifest().node_count != store.stats().corpus.logical_nodes) {
        return fail_v2(
            error,
            "logical node source v2/store manifest counts disagree");
    }

    LogicalNodeSourceV2ValidationStats local_stats{};
    for (;;) {
        LogicalNodeSourceNode node;
        bool has_node = false;
        if (!source.next(&node, &has_node, error)) {
            return false;
        }
        if (!has_node) {
            break;
        }

        std::uint64_t delivered = 0U;
        std::string span_error;
        if (!store.read_record_span(
                node.source_record_index,
                node.source_byte_offset,
                node.source_byte_length,
                [&](std::span<const std::byte> bytes) {
                    const std::uint64_t amount =
                        static_cast<std::uint64_t>(bytes.size());
                    if (delivered >
                        std::numeric_limits<std::uint64_t>::max() - amount) {
                        span_error = "logical node source v2 span byte counter overflows";
                        return false;
                    }
                    delivered += amount;
                    return true;
                },
                &span_error)) {
            return fail_v2(
                error,
                "logical node source v2 span escapes authoritative store: " + span_error);
        }
        if (!span_error.empty()) {
            return fail_v2(error, span_error);
        }
        if (delivered != node.source_byte_length) {
            return fail_v2(
                error,
                "logical node source v2 span delivery length disagrees with node metadata");
        }

        if (local_stats.nodes_validated ==
            std::numeric_limits<std::uint64_t>::max()) {
            return fail_v2(error, "logical node source v2 node counter overflows");
        }
        ++local_stats.nodes_validated;
        if (local_stats.attributes_validated >
            std::numeric_limits<std::uint64_t>::max() - node.attributes.size()) {
            return fail_v2(error, "logical node source v2 attribute counter overflows");
        }
        local_stats.attributes_validated +=
            static_cast<std::uint64_t>(node.attributes.size());
        if (local_stats.source_span_bytes_streamed >
            std::numeric_limits<std::uint64_t>::max() - delivered) {
            return fail_v2(error, "logical node source v2 streamed-byte counter overflows");
        }
        local_stats.source_span_bytes_streamed += delivered;
        if (node.source_byte_length == 0U) {
            if (local_stats.zero_length_spans ==
                std::numeric_limits<std::uint64_t>::max()) {
                return fail_v2(error, "logical node source v2 zero-span counter overflows");
            }
            ++local_stats.zero_length_spans;
        }
    }

    if (local_stats.nodes_validated != source.manifest().node_count ||
        local_stats.attributes_validated != source.manifest().attribute_count) {
        return fail_v2(
            error,
            "logical node source v2 validation totals disagree with manifest");
    }
    if (stats != nullptr) {
        *stats = local_stats;
    }
    return true;
}

} // namespace zevryon::massivedoc
