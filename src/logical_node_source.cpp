#include "logical_node_source.hpp"

#include "massivedoc_store.hpp"

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

constexpr std::array<std::uint8_t, 8> kSourceMagic{
    'Z', 'V', 'N', 'S', 'R', 'C', '0', '1'};
constexpr std::uint32_t kSourceFormatVersion = 1U;
constexpr std::uint32_t kSourceHeaderBytes = 72U;
constexpr std::uint32_t kNodeFrameFixedBytes = 64U;
constexpr std::uint32_t kNodeFrameMinimumBytes = kNodeFrameFixedBytes + 4U;
constexpr std::uint32_t kMaximumSemanticBytes = 1024U * 1024U;
constexpr std::uint32_t kMaximumNodeFrameBytes = 16U * 1024U * 1024U;

bool fail(std::string* error, std::string message) {
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

void store_u32(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned index = 0U; index < 4U; ++index) {
        (*bytes)[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
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

bool stream_count(std::size_t size, std::streamsize* output, std::string* error) {
    const auto maximum = static_cast<std::uintmax_t>(
        std::numeric_limits<std::streamsize>::max());
    if (static_cast<std::uintmax_t>(size) > maximum) {
        return fail(error, "logical node source I/O size exceeds stream limit");
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
        return fail(error, "logical node source write failed");
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
        return fail(error, "logical node source is truncated");
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
    bytes.insert(bytes.end(), manifest.source_sha256.begin(), manifest.source_sha256.end());
    append_u32(&bytes, 0U);
    append_u32(&bytes, crc32(std::span<const std::uint8_t>(bytes.data(), bytes.size())));
    return bytes;
}

bool decode_header(
    std::span<const std::uint8_t> bytes,
    LogicalNodeSourceManifest* manifest,
    std::string* error) {
    if (bytes.size() != kSourceHeaderBytes) {
        return fail(error, "logical node source header size mismatch");
    }
    if (!std::equal(kSourceMagic.begin(), kSourceMagic.end(), bytes.begin())) {
        return fail(error, "logical node source magic mismatch");
    }
    if (read_u32(bytes, 8U) != kSourceFormatVersion ||
        read_u32(bytes, 12U) != kSourceHeaderBytes) {
        return fail(error, "unsupported logical node source format");
    }
    if (read_u32(bytes, 64U) != 0U) {
        return fail(error, "logical node source reserved header bytes are nonzero");
    }
    if (crc32(bytes.first(68U)) != read_u32(bytes, 68U)) {
        return fail(error, "logical node source header CRC mismatch");
    }
    manifest->format_version = read_u32(bytes, 8U);
    manifest->node_count = read_u64(bytes, 16U);
    manifest->attribute_count = read_u64(bytes, 24U);
    std::copy_n(bytes.begin() + 32U, manifest->source_sha256.size(), manifest->source_sha256.begin());
    return true;
}

bool valid_semantic(std::string_view value) {
    return value.size() <= kMaximumSemanticBytes &&
        value.size() <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
}

std::filesystem::path building_path(const std::filesystem::path& output) {
    std::filesystem::path result = output;
    result += ".building";
    return result;
}

bool parse_sha256_hex(
    std::string_view hex,
    std::array<std::uint8_t, 32>* digest,
    std::string* error) {
    if (digest == nullptr || hex.size() != 64U) {
        return fail(error, "store payload SHA-256 is malformed");
    }
    const auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t index = 0U; index < digest->size(); ++index) {
        const int high = nibble(hex[index * 2U]);
        const int low = nibble(hex[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return fail(error, "store payload SHA-256 is malformed");
        }
        (*digest)[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

} // namespace

struct LogicalNodeSourceWriter::Impl {
    std::filesystem::path output;
    std::filesystem::path building;
    std::array<std::uint8_t, 32> source_sha256{};
    std::fstream stream;
    std::uint64_t node_count{0U};
    std::uint64_t attribute_count{0U};
    bool begun{false};
    bool finished{false};
    bool poisoned{false};

    Impl(
        std::filesystem::path output_path,
        std::array<std::uint8_t, 32> source_digest)
        : output(std::move(output_path)),
          building(building_path(output)),
          source_sha256(source_digest) {}
};

LogicalNodeSourceWriter::LogicalNodeSourceWriter(
    std::filesystem::path output_path,
    std::array<std::uint8_t, 32> source_sha256)
    : impl_(std::make_unique<Impl>(std::move(output_path), source_sha256)) {}

LogicalNodeSourceWriter::~LogicalNodeSourceWriter() {
    if (impl_ != nullptr && impl_->begun && !impl_->finished) {
        impl_->stream.close();
        std::error_code ignored;
        std::filesystem::remove(impl_->building, ignored);
    }
}

LogicalNodeSourceWriter::LogicalNodeSourceWriter(LogicalNodeSourceWriter&&) noexcept = default;
LogicalNodeSourceWriter& LogicalNodeSourceWriter::operator=(LogicalNodeSourceWriter&&) noexcept = default;

bool LogicalNodeSourceWriter::begin(std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (impl_->begun) {
        return fail(error, "logical node source writer already began");
    }
    if (impl_->output.empty()) {
        return fail(error, "logical node source output path is empty");
    }
    std::error_code fs_error;
    if (std::filesystem::exists(impl_->output, fs_error)) {
        if (fs_error) {
            return fail(error, "cannot inspect logical node source output: " + fs_error.message());
        }
        return fail(error, "logical node source output already exists");
    }
    fs_error.clear();
    std::filesystem::remove(impl_->building, fs_error);
    if (fs_error) {
        return fail(error, "cannot clear stale logical node source build: " + fs_error.message());
    }
    const std::filesystem::path parent = impl_->output.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, fs_error);
        if (fs_error) {
            return fail(error, "cannot create logical node source parent: " + fs_error.message());
        }
    }
    impl_->stream.open(
        impl_->building,
        std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!impl_->stream) {
        return fail(error, "cannot create logical node source build file");
    }
    LogicalNodeSourceManifest placeholder;
    placeholder.format_version = kSourceFormatVersion;
    placeholder.source_sha256 = impl_->source_sha256;
    const std::vector<std::uint8_t> header = encode_header(placeholder);
    if (header.size() != kSourceHeaderBytes ||
        !write_bytes(
            &impl_->stream,
            std::span<const std::uint8_t>(header.data(), header.size()),
            error)) {
        impl_->poisoned = true;
        return false;
    }
    impl_->begun = true;
    return true;
}

bool LogicalNodeSourceWriter::append_node(
    const LogicalNodeInput& node,
    std::span<const LogicalNodeAttributeInput> attributes,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    if (!impl_->begun || impl_->finished || impl_->poisoned) {
        return fail(error, "logical node source writer is not appendable");
    }
    const std::uint64_t ordinal = impl_->node_count;
    if (ordinal == std::numeric_limits<std::uint64_t>::max() ||
        node.logical_id != ordinal + 1U) {
        return fail(error, "logical node source ids must be contiguous 1-based pre-order ordinals");
    }
    if ((ordinal == 0U && node.parent_ordinal != kNoLogicalNodeOrdinal) ||
        (ordinal != 0U &&
         (node.parent_ordinal == kNoLogicalNodeOrdinal || node.parent_ordinal >= ordinal))) {
        return fail(error, "logical node source parent ordering is invalid");
    }
    if (node.tag.empty() || !valid_semantic(node.tag) ||
        !valid_semantic(node.role) || !valid_semantic(node.style)) {
        return fail(error, "logical node source semantic length is invalid");
    }
    if (node.source_byte_offset >
        std::numeric_limits<std::uint64_t>::max() - node.source_byte_length) {
        return fail(error, "logical node source byte range overflows");
    }
    if (attributes.size() > std::numeric_limits<std::uint32_t>::max()) {
        return fail(error, "logical node source attribute count exceeds fixed-width contract");
    }
    if (impl_->attribute_count >
        std::numeric_limits<std::uint64_t>::max() - attributes.size()) {
        return fail(error, "logical node source attribute count overflows");
    }
    for (const LogicalNodeAttributeInput& attribute : attributes) {
        if (attribute.name.empty() || !valid_semantic(attribute.name) ||
            !valid_semantic(attribute.value)) {
            return fail(error, "logical node source attribute semantic length is invalid");
        }
    }

    std::vector<std::uint8_t> frame;
    frame.reserve(kNodeFrameMinimumBytes + node.tag.size() + node.role.size() + node.style.size());
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
        if (frame.size() > kMaximumNodeFrameBytes - sizeof(std::uint32_t)) {
            impl_->poisoned = true;
            return fail(error, "logical node source frame exceeds bounded size");
        }
    }
    if (frame.size() > kMaximumNodeFrameBytes - sizeof(std::uint32_t)) {
        impl_->poisoned = true;
        return fail(error, "logical node source frame exceeds bounded size");
    }
    const std::uint32_t frame_bytes = static_cast<std::uint32_t>(
        frame.size() + sizeof(std::uint32_t));
    store_u32(&frame, 0U, frame_bytes);
    append_u32(&frame, crc32(std::span<const std::uint8_t>(frame.data(), frame.size())));
    if (!write_bytes(
            &impl_->stream,
            std::span<const std::uint8_t>(frame.data(), frame.size()),
            error)) {
        impl_->poisoned = true;
        return false;
    }
    ++impl_->node_count;
    impl_->attribute_count += static_cast<std::uint64_t>(attributes.size());
    return true;
}

bool LogicalNodeSourceWriter::finish(std::string* error) {
    if (error == nullptr) {
        return false;
    }
    if (!impl_->begun || impl_->finished || impl_->poisoned) {
        return fail(error, "logical node source writer cannot finish");
    }
    if (impl_->node_count == 0U) {
        return fail(error, "logical node source cannot publish an empty stream");
    }
    LogicalNodeSourceManifest manifest;
    manifest.format_version = kSourceFormatVersion;
    manifest.node_count = impl_->node_count;
    manifest.attribute_count = impl_->attribute_count;
    manifest.source_sha256 = impl_->source_sha256;
    const std::vector<std::uint8_t> header = encode_header(manifest);
    impl_->stream.flush();
    if (!impl_->stream) {
        impl_->poisoned = true;
        return fail(error, "logical node source flush failed");
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
        return fail(error, "logical node source header flush failed");
    }
    impl_->stream.close();

    std::error_code fs_error;
    if (std::filesystem::exists(impl_->output, fs_error)) {
        impl_->poisoned = true;
        if (fs_error) {
            return fail(error, "cannot recheck logical node source output: " + fs_error.message());
        }
        return fail(error, "logical node source output appeared during build");
    }
    std::filesystem::rename(impl_->building, impl_->output, fs_error);
    if (fs_error) {
        impl_->poisoned = true;
        return fail(error, "cannot publish logical node source: " + fs_error.message());
    }
    impl_->finished = true;
    return true;
}

std::uint64_t LogicalNodeSourceWriter::node_count() const noexcept {
    return impl_->node_count;
}

std::uint64_t LogicalNodeSourceWriter::attribute_count() const noexcept {
    return impl_->attribute_count;
}

struct LogicalNodeSourceReader::Impl {
    std::filesystem::path source;
    mutable std::ifstream stream;
    LogicalNodeSourceManifest manifest{};
    std::uint64_t consumed_nodes{0U};
    std::uint64_t consumed_attributes{0U};
    bool opened{false};
    bool ended{false};

    explicit Impl(std::filesystem::path source_path)
        : source(std::move(source_path)) {}
};

LogicalNodeSourceReader::LogicalNodeSourceReader(std::filesystem::path source_path)
    : impl_(std::make_unique<Impl>(std::move(source_path))) {}

LogicalNodeSourceReader::~LogicalNodeSourceReader() = default;
LogicalNodeSourceReader::LogicalNodeSourceReader(LogicalNodeSourceReader&&) noexcept = default;
LogicalNodeSourceReader& LogicalNodeSourceReader::operator=(LogicalNodeSourceReader&&) noexcept = default;

bool LogicalNodeSourceReader::open(std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (impl_->opened) {
        return fail(error, "logical node source reader already opened");
    }
    impl_->stream.open(impl_->source, std::ios::binary);
    if (!impl_->stream) {
        return fail(error, "cannot open logical node source");
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
        return fail(error, "logical node source manifest declares no nodes");
    }
    impl_->opened = true;
    return true;
}

const LogicalNodeSourceManifest& LogicalNodeSourceReader::manifest() const noexcept {
    return impl_->manifest;
}

bool LogicalNodeSourceReader::next(
    LogicalNodeSourceNode* node,
    bool* has_node,
    std::string* error) {
    if (node == nullptr || has_node == nullptr || error == nullptr) {
        return false;
    }
    *has_node = false;
    if (!impl_->opened || impl_->ended) {
        return fail(error, "logical node source reader is not readable");
    }
    if (impl_->consumed_nodes == impl_->manifest.node_count) {
        if (impl_->consumed_attributes != impl_->manifest.attribute_count) {
            return fail(error, "logical node source attribute count disagrees with manifest");
        }
        const int trailing = impl_->stream.peek();
        if (trailing != std::char_traits<char>::eof()) {
            return fail(error, "logical node source has trailing bytes");
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
    if (frame_bytes < kNodeFrameMinimumBytes || frame_bytes > kMaximumNodeFrameBytes) {
        return fail(error, "logical node source frame size is outside the bounded contract");
    }
    std::vector<std::uint8_t> frame(frame_bytes);
    std::copy(size_bytes.begin(), size_bytes.end(), frame.begin());
    if (!read_bytes(
            &impl_->stream,
            std::span<std::uint8_t>(frame.data() + size_bytes.size(), frame.size() - size_bytes.size()),
            error)) {
        return false;
    }
    const std::span<const std::uint8_t> view(frame.data(), frame.size());
    const std::size_t crc_offset = frame.size() - sizeof(std::uint32_t);
    if (crc32(view.first(crc_offset)) != read_u32(view, crc_offset)) {
        return fail(error, "logical node source frame CRC mismatch");
    }

    const std::uint32_t attribute_count = read_u32(view, 4U);
    LogicalNodeSourceNode output;
    output.logical_id = read_u64(view, 8U);
    output.source_record_index = read_u64(view, 16U);
    output.source_byte_offset = read_u64(view, 24U);
    output.source_byte_length = read_u64(view, 32U);
    output.parent_ordinal = read_u64(view, 40U);
    output.flags = read_u32(view, 48U);
    const std::uint32_t tag_bytes = read_u32(view, 52U);
    const std::uint32_t role_bytes = read_u32(view, 56U);
    const std::uint32_t style_bytes = read_u32(view, 60U);
    if (tag_bytes == 0U || tag_bytes > kMaximumSemanticBytes ||
        role_bytes > kMaximumSemanticBytes || style_bytes > kMaximumSemanticBytes ||
        output.source_byte_offset >
            std::numeric_limits<std::uint64_t>::max() - output.source_byte_length) {
        return fail(error, "logical node source frame has invalid semantic/range fields");
    }
    const std::uint64_t expected_id = impl_->consumed_nodes + 1U;
    if (output.logical_id != expected_id ||
        (impl_->consumed_nodes == 0U && output.parent_ordinal != kNoLogicalNodeOrdinal) ||
        (impl_->consumed_nodes != 0U &&
         (output.parent_ordinal == kNoLogicalNodeOrdinal ||
          output.parent_ordinal >= impl_->consumed_nodes))) {
        return fail(error, "logical node source pre-order identity is invalid");
    }

    std::size_t cursor = kNodeFrameFixedBytes;
    const auto take_string = [&](std::uint32_t length, std::string* value) -> bool {
        if (length > crc_offset - std::min(cursor, crc_offset)) {
            return false;
        }
        value->assign(
            reinterpret_cast<const char*>(frame.data() + cursor),
            static_cast<std::size_t>(length));
        cursor += static_cast<std::size_t>(length);
        return true;
    };
    if (!take_string(tag_bytes, &output.tag) ||
        !take_string(role_bytes, &output.role) ||
        !take_string(style_bytes, &output.style)) {
        return fail(error, "logical node source semantic payload is truncated");
    }

    output.attributes.reserve(attribute_count);
    for (std::uint32_t index = 0U; index < attribute_count; ++index) {
        if (cursor > crc_offset || crc_offset - cursor < 16U) {
            return fail(error, "logical node source attribute header is truncated");
        }
        const std::uint32_t name_bytes = read_u32(view, cursor);
        const std::uint32_t value_bytes = read_u32(view, cursor + 4U);
        const std::uint32_t flags = read_u32(view, cursor + 8U);
        const std::uint32_t reserved = read_u32(view, cursor + 12U);
        cursor += 16U;
        if (name_bytes == 0U || name_bytes > kMaximumSemanticBytes ||
            value_bytes > kMaximumSemanticBytes || reserved != 0U) {
            return fail(error, "logical node source attribute fields are invalid");
        }
        LogicalNodeSourceAttribute attribute;
        attribute.flags = flags;
        if (!take_string(name_bytes, &attribute.name) ||
            !take_string(value_bytes, &attribute.value)) {
            return fail(error, "logical node source attribute payload is truncated");
        }
        output.attributes.push_back(std::move(attribute));
    }
    if (cursor != crc_offset) {
        return fail(error, "logical node source frame payload length mismatch");
    }
    if (impl_->consumed_attributes >
        std::numeric_limits<std::uint64_t>::max() - attribute_count) {
        return fail(error, "logical node source consumed attribute count overflows");
    }
    impl_->consumed_attributes += attribute_count;
    if (impl_->consumed_attributes > impl_->manifest.attribute_count) {
        return fail(error, "logical node source attributes exceed manifest count");
    }
    ++impl_->consumed_nodes;
    *node = std::move(output);
    *has_node = true;
    error->clear();
    return true;
}

bool import_logical_node_source_to_arena(
    const std::filesystem::path& source_path,
    const std::filesystem::path& store_root,
    LogicalNodeSourceImportConfig config,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();

    StoreReader store(store_root);
    if (!store.open(error)) {
        return false;
    }
    LogicalNodeSourceReader source(source_path);
    if (!source.open(error)) {
        return false;
    }
    std::array<std::uint8_t, 32> store_digest{};
    if (!parse_sha256_hex(store.stats().payload_sha256, &store_digest, error)) {
        return false;
    }
    if (source.manifest().source_sha256 != store_digest) {
        return fail(error, "logical node source SHA-256 does not match store payload identity");
    }
    if (source.manifest().node_count != store.stats().corpus.logical_nodes) {
        return fail(error, "logical node source count disagrees with store logical_nodes metadata");
    }

    LogicalNodeArenaBuildConfig arena_config;
    arena_config.candidate_commit = std::move(config.candidate_commit);
    arena_config.candidate_tree = std::move(config.candidate_tree);
    arena_config.source_sha256 = store_digest;
    arena_config.semantic_bucket_count = config.semantic_bucket_count;
    arena_config.semantic_hash_bits = config.semantic_hash_bits;
    LogicalNodeArenaWriter arena(store_root, std::move(arena_config));
    if (!arena.begin(error)) {
        return false;
    }

    for (;;) {
        LogicalNodeSourceNode node;
        bool has_node = false;
        if (!source.next(&node, &has_node, error)) {
            return false;
        }
        if (!has_node) {
            break;
        }
        if (node.source_record_index >= store.stats().corpus.logical_records) {
            return fail(error, "logical node source record index is out of range");
        }
        const std::uint64_t range_end = node.source_byte_offset + node.source_byte_length;
        std::vector<std::byte> range_probe;
        std::string range_error;
        if (!store.read_record_slice(
                node.source_record_index,
                range_end,
                0U,
                &range_probe,
                &range_error)) {
            return fail(
                error,
                "logical node source byte range escapes its record: " + range_error);
        }

        std::vector<LogicalNodeAttributeInput> attributes;
        attributes.reserve(node.attributes.size());
        for (const LogicalNodeSourceAttribute& attribute : node.attributes) {
            attributes.push_back(
                LogicalNodeAttributeInput{attribute.name, attribute.value, attribute.flags});
        }
        if (!arena.append_node(
                LogicalNodeInput{
                    node.logical_id,
                    node.source_record_index,
                    node.source_byte_offset,
                    node.source_byte_length,
                    node.parent_ordinal,
                    node.tag,
                    node.role,
                    node.style,
                    node.flags},
                attributes,
                error)) {
            return false;
        }
    }
    return arena.finish(error);
}

} // namespace zevryon::massivedoc
