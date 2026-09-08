#include "logical_node_arena.hpp"

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
    'Z', 'V', 'N', 'O', 'D', 'A', '0', '1'};
constexpr std::uint32_t kFormatVersion = 1U;
constexpr std::uint64_t kNodeRecordSize = 96U;
constexpr std::uint64_t kAttributeRecordSize = 16U;
constexpr std::uint64_t kDictionaryHeaderSize = 32U;
constexpr std::uint64_t kManifestSize = 188U;
constexpr std::uint32_t kMaxSemanticBytes = 1024U * 1024U;
constexpr std::size_t kSemanticKindCount = 5U;
constexpr std::array<std::string_view, kSemanticKindCount> kSemanticPrefixes{
    "tag", "role", "style", "attribute-name", "attribute-value"};

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
        const unsigned shift = index * 8U;
        value |= static_cast<std::uint32_t>(bytes[offset + index]) << shift;
    }
    return value;
}

std::uint64_t read_u64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t value = 0U;
    for (unsigned index = 0U; index < 8U; ++index) {
        const unsigned shift = index * 8U;
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << shift;
    }
    return value;
}

void append_string_bytes(std::vector<std::uint8_t>* bytes, std::string_view value) {
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

std::uint64_t semantic_hash(std::string_view value, std::uint32_t hash_bits) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char character : value) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
        hash *= 1099511628211ULL;
    }
    if (hash_bits == 0U) {
        return 0U;
    }
    if (hash_bits < 64U) {
        const std::uint64_t mask = (std::uint64_t{1} << hash_bits) - 1U;
        hash &= mask;
    }
    return hash;
}

bool stream_count(std::size_t size, std::streamsize* value, std::string* error) {
    const auto maximum = static_cast<std::uintmax_t>(
        std::numeric_limits<std::streamsize>::max());
    if (static_cast<std::uintmax_t>(size) > maximum) {
        return fail(error, "logical node arena I/O size exceeds stream limit");
    }
    *value = static_cast<std::streamsize>(size);
    return true;
}

bool stream_offset(std::uint64_t offset, std::streamoff* value, std::string* error) {
    const auto maximum = static_cast<std::uint64_t>(
        std::numeric_limits<std::streamoff>::max());
    if (offset > maximum) {
        return fail(error, "logical node arena offset exceeds stream limit");
    }
    *value = static_cast<std::streamoff>(offset);
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
        return fail(error, "logical node arena write failed");
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
        return fail(error, "logical node arena read failed or was truncated");
    }
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
        return fail(error, "logical node arena read seek failed");
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
        return fail(error, "logical node arena write seek failed");
    }
    return true;
}

bool expected_file_size(
    std::uint64_t count,
    std::uint64_t record_size,
    std::uint64_t* result,
    std::string* error) {
    if (record_size != 0U && count > std::numeric_limits<std::uint64_t>::max() / record_size) {
        return fail(error, "logical node arena file size overflow");
    }
    *result = count * record_size;
    return true;
}

bool require_file_size(
    const std::filesystem::path& path,
    std::uint64_t expected,
    std::string* error) {
    std::error_code fs_error;
    const std::uintmax_t actual = std::filesystem::file_size(path, fs_error);
    if (fs_error) {
        return fail(error, "cannot stat logical node arena file: " + path.string() + ": " + fs_error.message());
    }
    if (actual != static_cast<std::uintmax_t>(expected)) {
        return fail(error, "logical node arena file size mismatch: " + path.string());
    }
    return true;
}

std::vector<std::uint8_t> encode_node_record(const LogicalNodeRecord& node) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(kNodeRecordSize));
    append_u64(&bytes, node.logical_id);
    append_u64(&bytes, node.source_record_index);
    append_u64(&bytes, node.source_byte_offset);
    append_u64(&bytes, node.source_byte_length);
    append_u64(&bytes, node.parent_ordinal);
    append_u64(&bytes, node.first_child_ordinal);
    append_u64(&bytes, node.next_sibling_ordinal);
    append_u64(&bytes, node.attribute_offset);
    append_u32(&bytes, node.attribute_count);
    append_u32(&bytes, node.tag_id);
    append_u32(&bytes, node.role_id);
    append_u32(&bytes, node.style_id);
    append_u32(&bytes, node.flags);
    append_u32(&bytes, 0U);
    append_u32(&bytes, 0U);
    append_u32(&bytes, crc32(std::span<const std::uint8_t>(bytes.data(), bytes.size())));
    return bytes;
}

bool decode_node_record(
    std::span<const std::uint8_t> bytes,
    LogicalNodeRecord* node,
    std::string* error) {
    if (bytes.size() != static_cast<std::size_t>(kNodeRecordSize)) {
        return fail(error, "logical node record size mismatch");
    }
    if (read_u32(bytes, 84U) != 0U || read_u32(bytes, 88U) != 0U) {
        return fail(error, "logical node reserved bytes are nonzero");
    }
    const std::uint32_t expected_crc = read_u32(bytes, 92U);
    if (crc32(bytes.first(92U)) != expected_crc) {
        return fail(error, "logical node record CRC mismatch");
    }
    node->logical_id = read_u64(bytes, 0U);
    node->source_record_index = read_u64(bytes, 8U);
    node->source_byte_offset = read_u64(bytes, 16U);
    node->source_byte_length = read_u64(bytes, 24U);
    node->parent_ordinal = read_u64(bytes, 32U);
    node->first_child_ordinal = read_u64(bytes, 40U);
    node->next_sibling_ordinal = read_u64(bytes, 48U);
    node->attribute_offset = read_u64(bytes, 56U);
    node->attribute_count = read_u32(bytes, 64U);
    node->tag_id = read_u32(bytes, 68U);
    node->role_id = read_u32(bytes, 72U);
    node->style_id = read_u32(bytes, 76U);
    node->flags = read_u32(bytes, 80U);
    return true;
}

std::vector<std::uint8_t> encode_attribute_record(const LogicalNodeAttributeRecord& attribute) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(kAttributeRecordSize));
    append_u32(&bytes, attribute.name_id);
    append_u32(&bytes, attribute.value_id);
    append_u32(&bytes, attribute.flags);
    append_u32(&bytes, crc32(std::span<const std::uint8_t>(bytes.data(), bytes.size())));
    return bytes;
}

bool decode_attribute_record(
    std::span<const std::uint8_t> bytes,
    LogicalNodeAttributeRecord* attribute,
    std::string* error) {
    if (bytes.size() != static_cast<std::size_t>(kAttributeRecordSize)) {
        return fail(error, "logical node attribute record size mismatch");
    }
    if (crc32(bytes.first(12U)) != read_u32(bytes, 12U)) {
        return fail(error, "logical node attribute CRC mismatch");
    }
    attribute->name_id = read_u32(bytes, 0U);
    attribute->value_id = read_u32(bytes, 4U);
    attribute->flags = read_u32(bytes, 8U);
    return true;
}

std::vector<std::uint8_t> encode_manifest(const LogicalNodeArenaManifest& manifest) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(kManifestSize));
    bytes.insert(bytes.end(), kManifestMagic.begin(), kManifestMagic.end());
    append_u32(&bytes, manifest.format_version);
    append_u32(&bytes, manifest.semantic_bucket_count);
    append_u32(&bytes, manifest.semantic_hash_bits);
    append_u32(&bytes, 0U);
    append_u64(&bytes, manifest.node_count);
    append_u64(&bytes, manifest.attribute_count);
    for (const std::uint32_t count : manifest.semantic_counts) {
        append_u32(&bytes, count);
    }
    append_u32(&bytes, 0U);
    append_string_bytes(&bytes, manifest.candidate_commit);
    append_string_bytes(&bytes, manifest.candidate_tree);
    bytes.insert(bytes.end(), manifest.source_sha256.begin(), manifest.source_sha256.end());
    append_u32(&bytes, static_cast<std::uint32_t>(kNodeRecordSize));
    append_u32(&bytes, static_cast<std::uint32_t>(kAttributeRecordSize));
    append_u32(&bytes, crc32(std::span<const std::uint8_t>(bytes.data(), bytes.size())));
    return bytes;
}

bool decode_manifest(
    std::span<const std::uint8_t> bytes,
    LogicalNodeArenaManifest* manifest,
    std::string* error) {
    if (bytes.size() != static_cast<std::size_t>(kManifestSize)) {
        return fail(error, "logical node arena manifest size mismatch");
    }
    if (!std::equal(kManifestMagic.begin(), kManifestMagic.end(), bytes.begin())) {
        return fail(error, "logical node arena manifest magic mismatch");
    }
    if (crc32(bytes.first(184U)) != read_u32(bytes, 184U)) {
        return fail(error, "logical node arena manifest CRC mismatch");
    }
    if (read_u32(bytes, 20U) != 0U || read_u32(bytes, 60U) != 0U) {
        return fail(error, "logical node arena manifest reserved bytes are nonzero");
    }
    if (read_u32(bytes, 176U) != static_cast<std::uint32_t>(kNodeRecordSize) ||
        read_u32(bytes, 180U) != static_cast<std::uint32_t>(kAttributeRecordSize)) {
        return fail(error, "logical node arena record-size contract mismatch");
    }
    manifest->format_version = read_u32(bytes, 8U);
    manifest->semantic_bucket_count = read_u32(bytes, 12U);
    manifest->semantic_hash_bits = read_u32(bytes, 16U);
    manifest->node_count = read_u64(bytes, 24U);
    manifest->attribute_count = read_u64(bytes, 32U);
    for (std::size_t index = 0U; index < manifest->semantic_counts.size(); ++index) {
        manifest->semantic_counts[index] = read_u32(bytes, 40U + index * 4U);
    }
    manifest->candidate_commit.assign(
        reinterpret_cast<const char*>(bytes.data() + 64U), 40U);
    manifest->candidate_tree.assign(
        reinterpret_cast<const char*>(bytes.data() + 104U), 40U);
    std::copy_n(bytes.begin() + 144U, manifest->source_sha256.size(), manifest->source_sha256.begin());
    if (manifest->format_version != kFormatVersion) {
        return fail(error, "unsupported logical node arena version");
    }
    if (!is_hex_sha(manifest->candidate_commit) || !is_hex_sha(manifest->candidate_tree)) {
        return fail(error, "logical node arena candidate identity is malformed");
    }
    if (manifest->semantic_bucket_count == 0U || manifest->semantic_bucket_count > 65536U ||
        manifest->semantic_hash_bits > 64U) {
        return fail(error, "logical node arena semantic configuration is invalid");
    }
    return true;
}

struct DictionaryEntry {
    std::uint32_t id{0U};
    std::uint64_t hash{0U};
    std::uint64_t next_offset_plus_one{0U};
    std::uint32_t length{0U};
    std::string value;
};

std::vector<std::uint8_t> dictionary_header_prefix(
    std::uint32_t id,
    std::uint64_t hash,
    std::uint64_t next_offset_plus_one,
    std::uint32_t length) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(28U);
    append_u32(&bytes, id);
    append_u32(&bytes, 0U);
    append_u64(&bytes, hash);
    append_u64(&bytes, next_offset_plus_one);
    append_u32(&bytes, length);
    return bytes;
}

class DiskInterner {
public:
    DiskInterner(
        std::filesystem::path directory,
        std::string prefix,
        std::uint32_t bucket_count,
        std::uint32_t hash_bits)
        : entries_path_(directory / (prefix + ".entries")),
          offsets_path_(directory / (prefix + ".offsets")),
          bucket_heads_(bucket_count, 0U),
          hash_bits_(hash_bits) {}

    bool open(std::string* error) {
        entries_.open(
            entries_path_,
            std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        offsets_.open(
            offsets_path_,
            std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        if (!entries_ || !offsets_) {
            return fail(error, "cannot create logical node semantic dictionary files");
        }
        return true;
    }

    bool intern(std::string_view value, std::uint32_t* id, std::string* error) {
        if (id == nullptr) {
            return fail(error, "logical node semantic output id is null");
        }
        if (value.size() > static_cast<std::size_t>(kMaxSemanticBytes) ||
            value.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            return fail(error, "logical node semantic value exceeds bounded length");
        }
        const std::uint64_t hash = semantic_hash(value, hash_bits_);
        const std::uint64_t bucket_modulus = static_cast<std::uint64_t>(bucket_heads_.size());
        const std::size_t bucket = static_cast<std::size_t>(hash % bucket_modulus);
        std::uint64_t cursor = bucket_heads_[bucket];
        std::uint32_t hops = 0U;
        while (cursor != 0U) {
            DictionaryEntry entry;
            if (!read_entry(cursor - 1U, &entry, error)) {
                return false;
            }
            if (entry.hash == hash && entry.value == value) {
                *id = entry.id;
                return true;
            }
            cursor = entry.next_offset_plus_one;
            ++hops;
            if (hops > count_) {
                return fail(error, "logical node semantic collision chain is cyclic");
            }
        }
        if (count_ == std::numeric_limits<std::uint32_t>::max()) {
            return fail(error, "logical node semantic id space exhausted");
        }
        const std::uint32_t next_id = count_ + 1U;
        const std::uint32_t length = static_cast<std::uint32_t>(value.size());
        std::vector<std::uint8_t> prefix = dictionary_header_prefix(
            next_id, hash, bucket_heads_[bucket], length);
        std::vector<std::uint8_t> crc_material = prefix;
        append_string_bytes(&crc_material, value);
        const std::uint32_t checksum = crc32(
            std::span<const std::uint8_t>(crc_material.data(), crc_material.size()));
        std::vector<std::uint8_t> header = prefix;
        append_u32(&header, checksum);

        if (!seek_write(&entries_, entry_end_, error) ||
            !write_bytes(
                &entries_,
                std::span<const std::uint8_t>(header.data(), header.size()),
                error)) {
            return false;
        }
        if (!value.empty()) {
            std::streamsize value_count = 0;
            if (!stream_count(value.size(), &value_count, error)) {
                return false;
            }
            entries_.write(value.data(), value_count);
            if (!entries_) {
                return fail(error, "logical node semantic payload write failed");
            }
        }

        std::vector<std::uint8_t> offset_bytes;
        offset_bytes.reserve(8U);
        append_u64(&offset_bytes, entry_end_ + 1U);
        const std::uint64_t offset_position = static_cast<std::uint64_t>(count_) * 8U;
        if (!seek_write(&offsets_, offset_position, error) ||
            !write_bytes(
                &offsets_,
                std::span<const std::uint8_t>(offset_bytes.data(), offset_bytes.size()),
                error)) {
            return false;
        }

        bucket_heads_[bucket] = entry_end_ + 1U;
        entry_end_ += kDictionaryHeaderSize + static_cast<std::uint64_t>(length);
        count_ = next_id;
        *id = next_id;
        return true;
    }

    bool flush_and_close(std::string* error) {
        entries_.flush();
        offsets_.flush();
        if (!entries_ || !offsets_) {
            return fail(error, "logical node semantic dictionary flush failed");
        }
        entries_.close();
        offsets_.close();
        return true;
    }

    std::uint32_t count() const noexcept {
        return count_;
    }

    std::uint64_t bucket_head_bytes() const noexcept {
        return static_cast<std::uint64_t>(bucket_heads_.size()) * sizeof(std::uint64_t);
    }

private:
    bool read_entry(std::uint64_t offset, DictionaryEntry* entry, std::string* error) {
        if (offset > entry_end_ || entry_end_ - offset < kDictionaryHeaderSize) {
            return fail(error, "logical node semantic dictionary offset is invalid");
        }
        std::array<std::uint8_t, static_cast<std::size_t>(kDictionaryHeaderSize)> header{};
        if (!seek_read(&entries_, offset, error) ||
            !read_bytes(&entries_, std::span<std::uint8_t>(header.data(), header.size()), error)) {
            return false;
        }
        const std::span<const std::uint8_t> header_view(header.data(), header.size());
        const std::uint32_t id = read_u32(header_view, 0U);
        if (id == 0U || read_u32(header_view, 4U) != 0U) {
            return fail(error, "logical node semantic dictionary header is corrupt");
        }
        const std::uint64_t hash = read_u64(header_view, 8U);
        const std::uint64_t next = read_u64(header_view, 16U);
        const std::uint32_t length = read_u32(header_view, 24U);
        const std::uint32_t expected_crc = read_u32(header_view, 28U);
        if (length > kMaxSemanticBytes ||
            offset + kDictionaryHeaderSize > entry_end_ ||
            static_cast<std::uint64_t>(length) > entry_end_ - offset - kDictionaryHeaderSize) {
            return fail(error, "logical node semantic dictionary payload is corrupt");
        }
        std::string value(static_cast<std::size_t>(length), '\0');
        if (length != 0U) {
            std::streamsize count = 0;
            if (!stream_count(value.size(), &count, error)) {
                return false;
            }
            entries_.read(value.data(), count);
            if (!entries_) {
                return fail(error, "logical node semantic dictionary payload is truncated");
            }
        }
        std::vector<std::uint8_t> crc_material = dictionary_header_prefix(id, hash, next, length);
        append_string_bytes(&crc_material, value);
        if (crc32(std::span<const std::uint8_t>(crc_material.data(), crc_material.size())) != expected_crc) {
            return fail(error, "logical node semantic dictionary CRC mismatch");
        }
        entry->id = id;
        entry->hash = hash;
        entry->next_offset_plus_one = next;
        entry->length = length;
        entry->value = std::move(value);
        return true;
    }

    std::filesystem::path entries_path_;
    std::filesystem::path offsets_path_;
    std::fstream entries_;
    std::fstream offsets_;
    std::vector<std::uint64_t> bucket_heads_;
    std::uint32_t hash_bits_{64U};
    std::uint32_t count_{0U};
    std::uint64_t entry_end_{0U};
};

class DiskDictionaryReader {
public:
    bool open(
        const std::filesystem::path& directory,
        std::string_view prefix,
        std::uint32_t count,
        std::string* error) {
        count_ = count;
        entries_path_ = directory / (std::string(prefix) + ".entries");
        offsets_path_ = directory / (std::string(prefix) + ".offsets");
        std::uint64_t expected_offsets = 0U;
        if (!expected_file_size(count_, 8U, &expected_offsets, error) ||
            !require_file_size(offsets_path_, expected_offsets, error)) {
            return false;
        }
        std::error_code fs_error;
        const std::uintmax_t size = std::filesystem::file_size(entries_path_, fs_error);
        if (fs_error || size > static_cast<std::uintmax_t>(std::numeric_limits<std::uint64_t>::max())) {
            return fail(error, "cannot stat logical node semantic entries file");
        }
        entries_size_ = static_cast<std::uint64_t>(size);
        entries_.open(entries_path_, std::ios::binary);
        offsets_.open(offsets_path_, std::ios::binary);
        if (!entries_ || !offsets_) {
            return fail(error, "cannot open logical node semantic dictionary");
        }
        return true;
    }

    bool resolve(std::uint32_t id, std::string* value, std::string* error) const {
        if (value == nullptr) {
            return fail(error, "logical node semantic output string is null");
        }
        if (id == 0U) {
            value->clear();
            return true;
        }
        if (id > count_) {
            return fail(error, "logical node semantic id is out of range");
        }
        const std::uint64_t offset_position = static_cast<std::uint64_t>(id - 1U) * 8U;
        std::array<std::uint8_t, 8> offset_bytes{};
        if (!seek_read(&offsets_, offset_position, error) ||
            !read_bytes(&offsets_, std::span<std::uint8_t>(offset_bytes.data(), offset_bytes.size()), error)) {
            return false;
        }
        const std::uint64_t offset_plus_one = read_u64(
            std::span<const std::uint8_t>(offset_bytes.data(), offset_bytes.size()), 0U);
        if (offset_plus_one == 0U) {
            return fail(error, "logical node semantic offset is zero");
        }
        const std::uint64_t offset = offset_plus_one - 1U;
        if (offset > entries_size_ || entries_size_ - offset < kDictionaryHeaderSize) {
            return fail(error, "logical node semantic offset escapes entries file");
        }
        std::array<std::uint8_t, static_cast<std::size_t>(kDictionaryHeaderSize)> header{};
        if (!seek_read(&entries_, offset, error) ||
            !read_bytes(&entries_, std::span<std::uint8_t>(header.data(), header.size()), error)) {
            return false;
        }
        const std::span<const std::uint8_t> header_view(header.data(), header.size());
        if (read_u32(header_view, 0U) != id || read_u32(header_view, 4U) != 0U) {
            return fail(error, "logical node semantic id/offset index mismatch");
        }
        const std::uint64_t hash = read_u64(header_view, 8U);
        const std::uint64_t next = read_u64(header_view, 16U);
        const std::uint32_t length = read_u32(header_view, 24U);
        const std::uint32_t expected_crc = read_u32(header_view, 28U);
        if (length > kMaxSemanticBytes ||
            static_cast<std::uint64_t>(length) > entries_size_ - offset - kDictionaryHeaderSize) {
            return fail(error, "logical node semantic payload length is invalid");
        }
        std::string decoded(static_cast<std::size_t>(length), '\0');
        if (length != 0U) {
            std::streamsize count = 0;
            if (!stream_count(decoded.size(), &count, error)) {
                return false;
            }
            entries_.read(decoded.data(), count);
            if (!entries_) {
                return fail(error, "logical node semantic payload is truncated");
            }
        }
        std::vector<std::uint8_t> crc_material = dictionary_header_prefix(id, hash, next, length);
        append_string_bytes(&crc_material, decoded);
        if (crc32(std::span<const std::uint8_t>(crc_material.data(), crc_material.size())) != expected_crc) {
            return fail(error, "logical node semantic payload CRC mismatch");
        }
        *value = std::move(decoded);
        return true;
    }

private:
    std::filesystem::path entries_path_;
    std::filesystem::path offsets_path_;
    mutable std::ifstream entries_;
    mutable std::ifstream offsets_;
    std::uint32_t count_{0U};
    std::uint64_t entries_size_{0U};
};

std::size_t semantic_index(LogicalSemanticKind kind) {
    return static_cast<std::size_t>(kind);
}

} // namespace

struct LogicalNodeArenaWriter::Impl {
    struct AncestorFrame {
        std::uint64_t ordinal{0U};
        std::uint64_t last_child{kNoLogicalNodeOrdinal};
    };

    std::filesystem::path root;
    std::filesystem::path building;
    std::filesystem::path published;
    LogicalNodeArenaBuildConfig config;
    std::fstream nodes;
    std::fstream attributes;
    std::array<std::unique_ptr<DiskInterner>, kSemanticKindCount> interners;
    std::vector<AncestorFrame> ancestors;
    std::uint64_t node_count{0U};
    std::uint64_t attribute_count{0U};
    bool begun{false};
    bool finished{false};
    bool poisoned{false};

    Impl(std::filesystem::path store_root, LogicalNodeArenaBuildConfig build_config)
        : root(std::move(store_root)),
          building(root / "node-arena.building"),
          published(root / "node-arena"),
          config(std::move(build_config)) {}

    bool poison(std::string* error, std::string message) {
        poisoned = true;
        return fail(error, std::move(message));
    }

    bool read_node(std::uint64_t ordinal, LogicalNodeRecord* node, std::string* error) {
        if (ordinal >= node_count) {
            return fail(error, "logical node patch ordinal is out of range");
        }
        std::uint64_t offset = 0U;
        if (!expected_file_size(ordinal, kNodeRecordSize, &offset, error)) {
            return false;
        }
        std::array<std::uint8_t, static_cast<std::size_t>(kNodeRecordSize)> bytes{};
        if (!seek_read(&nodes, offset, error) ||
            !read_bytes(&nodes, std::span<std::uint8_t>(bytes.data(), bytes.size()), error)) {
            return false;
        }
        return decode_node_record(
            std::span<const std::uint8_t>(bytes.data(), bytes.size()), node, error);
    }

    bool write_node(std::uint64_t ordinal, const LogicalNodeRecord& node, std::string* error) {
        std::uint64_t offset = 0U;
        if (!expected_file_size(ordinal, kNodeRecordSize, &offset, error)) {
            return false;
        }
        const std::vector<std::uint8_t> bytes = encode_node_record(node);
        if (bytes.size() != static_cast<std::size_t>(kNodeRecordSize)) {
            return fail(error, "logical node encoder produced wrong record size");
        }
        return seek_write(&nodes, offset, error) &&
            write_bytes(&nodes, std::span<const std::uint8_t>(bytes.data(), bytes.size()), error);
    }

    bool patch_child_link(
        std::uint64_t parent_ordinal,
        std::uint64_t previous_child,
        std::uint64_t child_ordinal,
        std::string* error) {
        const std::uint64_t target = previous_child == kNoLogicalNodeOrdinal
            ? parent_ordinal
            : previous_child;
        LogicalNodeRecord record;
        if (!read_node(target, &record, error)) {
            return false;
        }
        if (previous_child == kNoLogicalNodeOrdinal) {
            if (record.first_child_ordinal != kNoLogicalNodeOrdinal) {
                return fail(error, "logical node parent already has an unexpected first child");
            }
            record.first_child_ordinal = child_ordinal;
        } else {
            if (record.next_sibling_ordinal != kNoLogicalNodeOrdinal) {
                return fail(error, "logical node sibling already has an unexpected next sibling");
            }
            record.next_sibling_ordinal = child_ordinal;
        }
        return write_node(target, record, error);
    }
};

LogicalNodeArenaWriter::LogicalNodeArenaWriter(
    std::filesystem::path store_root,
    LogicalNodeArenaBuildConfig config)
    : impl_(std::make_unique<Impl>(std::move(store_root), std::move(config))) {}

LogicalNodeArenaWriter::~LogicalNodeArenaWriter() {
    if (impl_ != nullptr && impl_->begun && !impl_->finished) {
        impl_->nodes.close();
        impl_->attributes.close();
        std::error_code ignored;
        std::filesystem::remove_all(impl_->building, ignored);
    }
}

LogicalNodeArenaWriter::LogicalNodeArenaWriter(LogicalNodeArenaWriter&&) noexcept = default;
LogicalNodeArenaWriter& LogicalNodeArenaWriter::operator=(LogicalNodeArenaWriter&&) noexcept = default;

bool LogicalNodeArenaWriter::begin(std::string* error) {
    if (impl_->begun) {
        return fail(error, "logical node arena writer already began");
    }
    if (!is_hex_sha(impl_->config.candidate_commit) || !is_hex_sha(impl_->config.candidate_tree)) {
        return fail(error, "logical node arena candidate commit/tree must be 40 hex characters");
    }
    if (impl_->config.semantic_bucket_count == 0U || impl_->config.semantic_bucket_count > 65536U ||
        impl_->config.semantic_hash_bits > 64U) {
        return fail(error, "logical node arena semantic configuration is invalid");
    }
    std::error_code fs_error;
    if (std::filesystem::exists(impl_->published, fs_error)) {
        if (fs_error) {
            return fail(error, "cannot inspect existing logical node arena: " + fs_error.message());
        }
        return fail(error, "authoritative logical node arena already exists");
    }
    fs_error.clear();
    if (std::filesystem::exists(impl_->building, fs_error)) {
        if (fs_error) {
            return fail(error, "cannot inspect stale logical node arena build: " + fs_error.message());
        }
        std::filesystem::remove_all(impl_->building, fs_error);
        if (fs_error) {
            return fail(error, "cannot clear stale logical node arena build: " + fs_error.message());
        }
    }
    std::filesystem::create_directories(impl_->building, fs_error);
    if (fs_error) {
        return fail(error, "cannot create logical node arena build directory: " + fs_error.message());
    }
    impl_->nodes.open(
        impl_->building / "nodes.bin",
        std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    impl_->attributes.open(
        impl_->building / "attributes.bin",
        std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!impl_->nodes || !impl_->attributes) {
        return fail(error, "cannot create logical node arena record files");
    }
    for (std::size_t index = 0U; index < kSemanticKindCount; ++index) {
        impl_->interners[index] = std::make_unique<DiskInterner>(
            impl_->building,
            std::string(kSemanticPrefixes[index]),
            impl_->config.semantic_bucket_count,
            impl_->config.semantic_hash_bits);
        if (!impl_->interners[index]->open(error)) {
            return false;
        }
    }
    impl_->begun = true;
    return true;
}

bool LogicalNodeArenaWriter::append_node(
    const LogicalNodeInput& input,
    std::span<const LogicalNodeAttributeInput> input_attributes,
    std::string* error) {
    if (!impl_->begun || impl_->finished || impl_->poisoned) {
        return fail(error, "logical node arena writer is not appendable");
    }
    const std::uint64_t ordinal = impl_->node_count;
    if (ordinal == std::numeric_limits<std::uint64_t>::max() || input.logical_id != ordinal + 1U) {
        return fail(error, "logical node ids must be contiguous 1-based pre-order ordinals");
    }
    if (input.tag.empty()) {
        return fail(error, "logical node tag cannot be empty");
    }
    if (input.source_byte_offset > std::numeric_limits<std::uint64_t>::max() - input.source_byte_length) {
        return fail(error, "logical node source byte range overflows");
    }
    if (input_attributes.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return fail(error, "logical node attribute count exceeds fixed-width contract");
    }
    if (impl_->attribute_count > std::numeric_limits<std::uint64_t>::max() - input_attributes.size()) {
        return fail(error, "logical node attribute arena overflows");
    }

    if (ordinal == 0U) {
        if (input.parent_ordinal != kNoLogicalNodeOrdinal) {
            return fail(error, "logical node root must not have a parent");
        }
    } else {
        if (input.parent_ordinal == kNoLogicalNodeOrdinal || input.parent_ordinal >= ordinal) {
            return fail(error, "logical node parent must precede the node in pre-order");
        }
        while (!impl_->ancestors.empty() &&
               impl_->ancestors.back().ordinal != input.parent_ordinal) {
            impl_->ancestors.pop_back();
        }
        if (impl_->ancestors.empty()) {
            return fail(error, "logical node parent is not an open pre-order ancestor");
        }
    }

    std::uint32_t tag_id = 0U;
    std::uint32_t role_id = 0U;
    std::uint32_t style_id = 0U;
    if (!impl_->interners[semantic_index(LogicalSemanticKind::tag)]->intern(input.tag, &tag_id, error)) {
        impl_->poisoned = true;
        return false;
    }
    if (!input.role.empty() &&
        !impl_->interners[semantic_index(LogicalSemanticKind::role)]->intern(input.role, &role_id, error)) {
        impl_->poisoned = true;
        return false;
    }
    if (!input.style.empty() &&
        !impl_->interners[semantic_index(LogicalSemanticKind::style)]->intern(input.style, &style_id, error)) {
        impl_->poisoned = true;
        return false;
    }

    const std::uint64_t attribute_offset = impl_->attribute_count;
    for (const LogicalNodeAttributeInput& attribute : input_attributes) {
        if (attribute.name.empty()) {
            return impl_->poison(error, "logical node attribute name cannot be empty");
        }
        std::uint32_t name_id = 0U;
        std::uint32_t value_id = 0U;
        if (!impl_->interners[semantic_index(LogicalSemanticKind::attribute_name)]->intern(
                attribute.name, &name_id, error) ||
            !impl_->interners[semantic_index(LogicalSemanticKind::attribute_value)]->intern(
                attribute.value, &value_id, error)) {
            impl_->poisoned = true;
            return false;
        }
        const LogicalNodeAttributeRecord record{name_id, value_id, attribute.flags};
        const std::vector<std::uint8_t> bytes = encode_attribute_record(record);
        std::uint64_t write_offset = 0U;
        if (!expected_file_size(impl_->attribute_count, kAttributeRecordSize, &write_offset, error) ||
            !seek_write(&impl_->attributes, write_offset, error) ||
            !write_bytes(
                &impl_->attributes,
                std::span<const std::uint8_t>(bytes.data(), bytes.size()),
                error)) {
            impl_->poisoned = true;
            return false;
        }
        ++impl_->attribute_count;
    }

    LogicalNodeRecord node;
    node.logical_id = input.logical_id;
    node.source_record_index = input.source_record_index;
    node.source_byte_offset = input.source_byte_offset;
    node.source_byte_length = input.source_byte_length;
    node.parent_ordinal = input.parent_ordinal;
    node.attribute_offset = attribute_offset;
    node.attribute_count = static_cast<std::uint32_t>(input_attributes.size());
    node.tag_id = tag_id;
    node.role_id = role_id;
    node.style_id = style_id;
    node.flags = input.flags;
    if (!impl_->write_node(ordinal, node, error)) {
        impl_->poisoned = true;
        return false;
    }

    if (ordinal != 0U) {
        Impl::AncestorFrame& parent = impl_->ancestors.back();
        if (!impl_->patch_child_link(
                parent.ordinal,
                parent.last_child,
                ordinal,
                error)) {
            impl_->poisoned = true;
            return false;
        }
        parent.last_child = ordinal;
    }
    ++impl_->node_count;
    impl_->ancestors.push_back(Impl::AncestorFrame{ordinal, kNoLogicalNodeOrdinal});
    return true;
}

bool LogicalNodeArenaWriter::finish(std::string* error) {
    if (!impl_->begun || impl_->finished || impl_->poisoned) {
        return fail(error, "logical node arena writer cannot finish");
    }
    if (impl_->node_count == 0U) {
        return fail(error, "logical node arena cannot publish without a root node");
    }
    impl_->nodes.flush();
    impl_->attributes.flush();
    if (!impl_->nodes || !impl_->attributes) {
        impl_->poisoned = true;
        return fail(error, "logical node arena record flush failed");
    }
    impl_->nodes.close();
    impl_->attributes.close();
    for (const auto& interner : impl_->interners) {
        if (!interner->flush_and_close(error)) {
            impl_->poisoned = true;
            return false;
        }
    }

    LogicalNodeArenaManifest manifest;
    manifest.format_version = kFormatVersion;
    manifest.semantic_bucket_count = impl_->config.semantic_bucket_count;
    manifest.semantic_hash_bits = impl_->config.semantic_hash_bits;
    manifest.node_count = impl_->node_count;
    manifest.attribute_count = impl_->attribute_count;
    for (std::size_t index = 0U; index < kSemanticKindCount; ++index) {
        manifest.semantic_counts[index] = impl_->interners[index]->count();
    }
    manifest.candidate_commit = impl_->config.candidate_commit;
    manifest.candidate_tree = impl_->config.candidate_tree;
    manifest.source_sha256 = impl_->config.source_sha256;
    const std::vector<std::uint8_t> manifest_bytes = encode_manifest(manifest);
    if (manifest_bytes.size() != static_cast<std::size_t>(kManifestSize)) {
        impl_->poisoned = true;
        return fail(error, "logical node arena manifest encoder produced wrong size");
    }
    std::ofstream manifest_stream(
        impl_->building / "manifest.bin",
        std::ios::binary | std::ios::trunc);
    if (!manifest_stream ||
        !write_bytes(
            &manifest_stream,
            std::span<const std::uint8_t>(manifest_bytes.data(), manifest_bytes.size()),
            error)) {
        impl_->poisoned = true;
        return false;
    }
    manifest_stream.flush();
    if (!manifest_stream) {
        impl_->poisoned = true;
        return fail(error, "logical node arena manifest flush failed");
    }
    manifest_stream.close();

    std::error_code fs_error;
    if (std::filesystem::exists(impl_->published, fs_error)) {
        if (fs_error) {
            impl_->poisoned = true;
            return fail(error, "cannot recheck logical node arena publication path: " + fs_error.message());
        }
        impl_->poisoned = true;
        return fail(error, "authoritative logical node arena appeared during build");
    }
    std::filesystem::rename(impl_->building, impl_->published, fs_error);
    if (fs_error) {
        impl_->poisoned = true;
        return fail(error, "cannot publish logical node arena: " + fs_error.message());
    }
    impl_->finished = true;
    return true;
}

std::uint64_t LogicalNodeArenaWriter::node_count() const noexcept {
    return impl_->node_count;
}

std::uint64_t LogicalNodeArenaWriter::attribute_count() const noexcept {
    return impl_->attribute_count;
}

std::uint64_t LogicalNodeArenaWriter::semantic_bucket_head_bytes() const noexcept {
    std::uint64_t total = 0U;
    for (const auto& interner : impl_->interners) {
        if (interner != nullptr) {
            total += interner->bucket_head_bytes();
        }
    }
    return total;
}

struct LogicalNodeArenaReader::Impl {
    std::filesystem::path root;
    std::filesystem::path arena;
    LogicalNodeArenaManifest manifest;
    mutable std::ifstream nodes;
    mutable std::ifstream attributes;
    std::array<DiskDictionaryReader, kSemanticKindCount> dictionaries;
    bool opened{false};

    explicit Impl(std::filesystem::path store_root)
        : root(std::move(store_root)), arena(root / "node-arena") {}

    bool validate_node(
        std::uint64_t ordinal,
        const LogicalNodeRecord& node,
        std::string* error) const {
        if (node.logical_id != ordinal + 1U) {
            return fail(error, "logical node id/ordinal contract mismatch");
        }
        if (ordinal == 0U) {
            if (node.parent_ordinal != kNoLogicalNodeOrdinal) {
                return fail(error, "logical node root has a parent");
            }
        } else if (node.parent_ordinal == kNoLogicalNodeOrdinal || node.parent_ordinal >= ordinal) {
            return fail(error, "logical node parent topology is invalid");
        }
        if (node.first_child_ordinal != kNoLogicalNodeOrdinal &&
            (node.first_child_ordinal <= ordinal || node.first_child_ordinal >= manifest.node_count)) {
            return fail(error, "logical node first-child topology is invalid");
        }
        if (node.next_sibling_ordinal != kNoLogicalNodeOrdinal &&
            (node.next_sibling_ordinal <= ordinal || node.next_sibling_ordinal >= manifest.node_count)) {
            return fail(error, "logical node next-sibling topology is invalid");
        }
        if (node.attribute_offset > manifest.attribute_count ||
            static_cast<std::uint64_t>(node.attribute_count) >
                manifest.attribute_count - node.attribute_offset) {
            return fail(error, "logical node attribute slice escapes attribute arena");
        }
        if (node.source_byte_offset > std::numeric_limits<std::uint64_t>::max() - node.source_byte_length) {
            return fail(error, "logical node source range overflows");
        }
        if (node.tag_id == 0U || node.tag_id > manifest.semantic_counts[semantic_index(LogicalSemanticKind::tag)] ||
            node.role_id > manifest.semantic_counts[semantic_index(LogicalSemanticKind::role)] ||
            node.style_id > manifest.semantic_counts[semantic_index(LogicalSemanticKind::style)]) {
            return fail(error, "logical node semantic id is out of range");
        }
        return true;
    }
};

LogicalNodeArenaReader::LogicalNodeArenaReader(std::filesystem::path store_root)
    : impl_(std::make_unique<Impl>(std::move(store_root))) {}

LogicalNodeArenaReader::~LogicalNodeArenaReader() = default;
LogicalNodeArenaReader::LogicalNodeArenaReader(LogicalNodeArenaReader&&) noexcept = default;
LogicalNodeArenaReader& LogicalNodeArenaReader::operator=(LogicalNodeArenaReader&&) noexcept = default;

bool LogicalNodeArenaReader::open(std::string* error) {
    if (impl_->opened) {
        return fail(error, "logical node arena reader already opened");
    }
    if (!require_file_size(impl_->arena / "manifest.bin", kManifestSize, error)) {
        return false;
    }
    std::ifstream manifest_stream(impl_->arena / "manifest.bin", std::ios::binary);
    if (!manifest_stream) {
        return fail(error, "cannot open logical node arena manifest");
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
    if (impl_->manifest.node_count == 0U) {
        return fail(error, "logical node arena manifest contains no root node");
    }
    std::uint64_t expected_nodes = 0U;
    std::uint64_t expected_attributes = 0U;
    if (!expected_file_size(impl_->manifest.node_count, kNodeRecordSize, &expected_nodes, error) ||
        !expected_file_size(impl_->manifest.attribute_count, kAttributeRecordSize, &expected_attributes, error) ||
        !require_file_size(impl_->arena / "nodes.bin", expected_nodes, error) ||
        !require_file_size(impl_->arena / "attributes.bin", expected_attributes, error)) {
        return false;
    }
    impl_->nodes.open(impl_->arena / "nodes.bin", std::ios::binary);
    impl_->attributes.open(impl_->arena / "attributes.bin", std::ios::binary);
    if (!impl_->nodes || !impl_->attributes) {
        return fail(error, "cannot open logical node arena record files");
    }
    for (std::size_t index = 0U; index < kSemanticKindCount; ++index) {
        if (!impl_->dictionaries[index].open(
                impl_->arena,
                kSemanticPrefixes[index],
                impl_->manifest.semantic_counts[index],
                error)) {
            return false;
        }
    }
    LogicalNodeRecord root;
    if (!node_by_ordinal(0U, &root, error)) {
        return false;
    }
    impl_->opened = true;
    return true;
}

const LogicalNodeArenaManifest& LogicalNodeArenaReader::manifest() const noexcept {
    return impl_->manifest;
}

bool LogicalNodeArenaReader::node_by_ordinal(
    std::uint64_t ordinal,
    LogicalNodeRecord* node,
    std::string* error) const {
    if (node == nullptr) {
        return fail(error, "logical node output record is null");
    }
    if (ordinal >= impl_->manifest.node_count) {
        return fail(error, "logical node ordinal is out of range");
    }
    std::uint64_t offset = 0U;
    if (!expected_file_size(ordinal, kNodeRecordSize, &offset, error)) {
        return false;
    }
    std::array<std::uint8_t, static_cast<std::size_t>(kNodeRecordSize)> bytes{};
    if (!seek_read(&impl_->nodes, offset, error) ||
        !read_bytes(&impl_->nodes, std::span<std::uint8_t>(bytes.data(), bytes.size()), error) ||
        !decode_node_record(
            std::span<const std::uint8_t>(bytes.data(), bytes.size()), node, error)) {
        return false;
    }
    return impl_->validate_node(ordinal, *node, error);
}

bool LogicalNodeArenaReader::node_by_id(
    std::uint64_t logical_id,
    LogicalNodeRecord* node,
    std::string* error) const {
    if (logical_id == 0U || logical_id > impl_->manifest.node_count) {
        return fail(error, "logical node id is out of range");
    }
    return node_by_ordinal(logical_id - 1U, node, error);
}

bool LogicalNodeArenaReader::attribute_by_ordinal(
    std::uint64_t ordinal,
    LogicalNodeAttributeRecord* attribute,
    std::string* error) const {
    if (attribute == nullptr) {
        return fail(error, "logical node attribute output is null");
    }
    if (ordinal >= impl_->manifest.attribute_count) {
        return fail(error, "logical node attribute ordinal is out of range");
    }
    std::uint64_t offset = 0U;
    if (!expected_file_size(ordinal, kAttributeRecordSize, &offset, error)) {
        return false;
    }
    std::array<std::uint8_t, static_cast<std::size_t>(kAttributeRecordSize)> bytes{};
    if (!seek_read(&impl_->attributes, offset, error) ||
        !read_bytes(&impl_->attributes, std::span<std::uint8_t>(bytes.data(), bytes.size()), error) ||
        !decode_attribute_record(
            std::span<const std::uint8_t>(bytes.data(), bytes.size()), attribute, error)) {
        return false;
    }
    const std::uint32_t name_count =
        impl_->manifest.semantic_counts[semantic_index(LogicalSemanticKind::attribute_name)];
    const std::uint32_t value_count =
        impl_->manifest.semantic_counts[semantic_index(LogicalSemanticKind::attribute_value)];
    if (attribute->name_id == 0U || attribute->name_id > name_count ||
        attribute->value_id == 0U || attribute->value_id > value_count) {
        return fail(error, "logical node attribute semantic id is out of range");
    }
    return true;
}

bool LogicalNodeArenaReader::resolve_semantic(
    LogicalSemanticKind kind,
    std::uint32_t id,
    std::string* value,
    std::string* error) const {
    const std::size_t index = semantic_index(kind);
    if (index >= kSemanticKindCount) {
        return fail(error, "logical node semantic kind is invalid");
    }
    return impl_->dictionaries[index].resolve(id, value, error);
}

} // namespace zevryon::massivedoc
