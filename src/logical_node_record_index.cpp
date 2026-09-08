#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif

#include "logical_node_record_index.hpp"

#include "font_content_identity.hpp"
#include "logical_node_arena_v2_store_bound.hpp"
#include "massivedoc_positional_io.hpp"
#include "massivedoc_store.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace zevryon::massivedoc {
namespace {

constexpr std::array<std::uint8_t, 8> kManifestMagic{
    'Z', 'V', 'N', 'R', 'I', 'D', 'X', '1'};
constexpr std::size_t kManifestBytes = 152U;
constexpr std::size_t kRecordEntryBytes = 24U;
constexpr std::size_t kHeadEntryBytes = 32U;
constexpr std::size_t kPostingEntryBytes = 48U;
constexpr std::size_t kStoreRecordDescriptorBytes = 32U;
constexpr std::size_t kStoreRecordLengthOffset = 16U;
constexpr std::string_view kArenaIdentityDomain =
    "ZEVRYON-NODE-RECORD-INDEX-ARENA-V1";

static_assert(
    sizeof(std::streamoff) >= sizeof(std::int64_t),
    "logical-node record-index builder requires 64-bit stream offsets");

struct RecordEntry {
    std::uint64_t absolute_start{0U};
    std::uint64_t byte_length{0U};
};

struct HeadEntry {
    std::uint64_t first_posting{kNoLogicalNodeRecordPosting};
    std::uint64_t last_posting{kNoLogicalNodeRecordPosting};
    std::uint64_t posting_count{0U};
};

struct PostingEntry {
    std::uint64_t node_ordinal{0U};
    std::uint64_t next_posting{kNoLogicalNodeRecordPosting};
    std::uint64_t source_record_index{0U};
    std::uint64_t record_byte_offset{0U};
    std::uint64_t record_byte_length{0U};
};

bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

template <typename T, std::size_t N>
void put_le(std::array<std::uint8_t, N>* output, std::size_t offset, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        (*output)[offset + index] = static_cast<std::uint8_t>(
            (value >> (index * 8U)) & static_cast<T>(0xffU));
    }
}

template <typename T>
T get_le(std::span<const std::uint8_t> input, std::size_t offset) {
    static_assert(std::is_unsigned_v<T>);
    T value = 0U;
    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        value |= static_cast<T>(input[offset + index]) << (index * 8U);
    }
    return value;
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t byte : bytes) {
        crc ^= static_cast<std::uint32_t>(byte);
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const std::uint32_t mask =
                static_cast<std::uint32_t>(0U - (crc & 1U));
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

template <std::size_t N>
std::span<const std::byte> as_bytes(const std::array<std::uint8_t, N>& input) {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(input.data()),
        input.size());
}

template <std::size_t N>
std::span<std::byte> as_writable_bytes(std::array<std::uint8_t, N>* input) {
    return std::span<std::byte>(
        reinterpret_cast<std::byte*>(input->data()),
        input->size());
}

bool checked_mul(
    std::uint64_t count,
    std::size_t entry_bytes,
    std::uint64_t* result,
    std::string* error) {
    if (result == nullptr) {
        return false;
    }
    const std::uint64_t width = static_cast<std::uint64_t>(entry_bytes);
    if (count > std::numeric_limits<std::uint64_t>::max() / width) {
        return fail(error, "logical-node record-index table size overflows");
    }
    *result = count * width;
    return true;
}

bool checked_add(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t* result,
    std::string* error) {
    if (result == nullptr) {
        return false;
    }
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return fail(error, "logical-node record-index source position overflows");
    }
    *result = left + right;
    return true;
}

bool source_binding_equal(
    const LogicalNodeSourceStoreBinding& left,
    const LogicalNodeSourceStoreBinding& right) noexcept {
    return left.source_record_count == right.source_record_count &&
        left.payload_sha256 == right.payload_sha256 &&
        left.record_sequence_sha256 == right.record_sequence_sha256;
}

bool sha_update(
    zevryon::text::Sha256* sha,
    std::span<const std::byte> bytes,
    std::string* error) {
    if (sha == nullptr || !sha->update(bytes)) {
        return fail(
            error,
            "logical-node record-index arena identity exceeds SHA-256 input bounds");
    }
    return true;
}

template <typename T>
bool sha_update_le(
    zevryon::text::Sha256* sha,
    T value,
    std::string* error) {
    static_assert(std::is_unsigned_v<T>);
    std::array<std::uint8_t, sizeof(T)> encoded{};
    put_le(&encoded, 0U, value);
    return sha_update(sha, as_bytes(encoded), error);
}

bool sha_update_string(
    zevryon::text::Sha256* sha,
    std::string_view value,
    std::string* error) {
    if (!sha_update_le(
            sha,
            static_cast<std::uint64_t>(value.size()),
            error)) {
        return false;
    }
    return sha_update(
        sha,
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(value.data()),
            value.size()),
        error);
}

bool compute_arena_identity(
    const LogicalNodeArenaV2Manifest& manifest,
    std::array<std::uint8_t, 32>* digest,
    std::string* error) {
    if (digest == nullptr || error == nullptr) {
        return false;
    }
    zevryon::text::Sha256 sha;
    if (!sha_update(
            &sha,
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(kArenaIdentityDomain.data()),
                kArenaIdentityDomain.size()),
            error) ||
        !sha_update_le(&sha, manifest.format_version, error) ||
        !sha_update_le(&sha, manifest.storage_manifest.format_version, error) ||
        !sha_update_le(
            &sha, manifest.storage_manifest.semantic_bucket_count, error) ||
        !sha_update_le(
            &sha, manifest.storage_manifest.semantic_hash_bits, error) ||
        !sha_update_le(&sha, manifest.storage_manifest.node_count, error) ||
        !sha_update_le(&sha, manifest.storage_manifest.attribute_count, error)) {
        return false;
    }
    for (const std::uint32_t count : manifest.storage_manifest.semantic_counts) {
        if (!sha_update_le(&sha, count, error)) {
            return false;
        }
    }
    if (!sha_update_string(
            &sha, manifest.storage_manifest.candidate_commit, error) ||
        !sha_update_string(
            &sha, manifest.storage_manifest.candidate_tree, error) ||
        !sha_update(
            &sha,
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(
                    manifest.storage_manifest.source_sha256.data()),
                manifest.storage_manifest.source_sha256.size()),
            error)) {
        return false;
    }

    zevryon::text::Sha256Digest result{};
    if (!sha.finish(&result)) {
        return fail(
            error,
            "cannot finalize logical-node record-index arena identity");
    }
    for (std::size_t index = 0U; index < digest->size(); ++index) {
        (*digest)[index] = std::to_integer<std::uint8_t>(result[index]);
    }
    return true;
}

std::array<std::uint8_t, kRecordEntryBytes> encode_record_entry(
    const RecordEntry& entry) {
    std::array<std::uint8_t, kRecordEntryBytes> bytes{};
    put_le(&bytes, 0U, entry.absolute_start);
    put_le(&bytes, 8U, entry.byte_length);
    put_le(&bytes, 16U, std::uint32_t{0U});
    put_le(
        &bytes,
        20U,
        crc32(std::span<const std::uint8_t>(bytes.data(), 20U)));
    return bytes;
}

bool decode_record_entry(
    std::span<const std::uint8_t> bytes,
    RecordEntry* entry,
    std::string* error) {
    if (entry == nullptr || bytes.size() != kRecordEntryBytes) {
        return fail(
            error,
            "logical-node record-index record entry has invalid size");
    }
    if (get_le<std::uint32_t>(bytes, 16U) != 0U ||
        crc32(bytes.first(20U)) != get_le<std::uint32_t>(bytes, 20U)) {
        return fail(
            error,
            "logical-node record-index record entry CRC/reserved mismatch");
    }
    entry->absolute_start = get_le<std::uint64_t>(bytes, 0U);
    entry->byte_length = get_le<std::uint64_t>(bytes, 8U);
    return true;
}

std::array<std::uint8_t, kHeadEntryBytes> encode_head_entry(
    const HeadEntry& entry) {
    std::array<std::uint8_t, kHeadEntryBytes> bytes{};
    put_le(&bytes, 0U, entry.first_posting);
    put_le(&bytes, 8U, entry.last_posting);
    put_le(&bytes, 16U, entry.posting_count);
    put_le(&bytes, 24U, std::uint32_t{0U});
    put_le(
        &bytes,
        28U,
        crc32(std::span<const std::uint8_t>(bytes.data(), 28U)));
    return bytes;
}

bool decode_head_entry(
    std::span<const std::uint8_t> bytes,
    HeadEntry* entry,
    std::string* error) {
    if (entry == nullptr || bytes.size() != kHeadEntryBytes) {
        return fail(
            error,
            "logical-node record-index head entry has invalid size");
    }
    if (get_le<std::uint32_t>(bytes, 24U) != 0U ||
        crc32(bytes.first(28U)) != get_le<std::uint32_t>(bytes, 28U)) {
        return fail(
            error,
            "logical-node record-index head entry CRC/reserved mismatch");
    }
    entry->first_posting = get_le<std::uint64_t>(bytes, 0U);
    entry->last_posting = get_le<std::uint64_t>(bytes, 8U);
    entry->posting_count = get_le<std::uint64_t>(bytes, 16U);
    const bool empty = entry->posting_count == 0U;
    if (empty !=
        (entry->first_posting == kNoLogicalNodeRecordPosting &&
         entry->last_posting == kNoLogicalNodeRecordPosting)) {
        return fail(
            error,
            "logical-node record-index head empty-state mismatch");
    }
    return true;
}

std::array<std::uint8_t, kPostingEntryBytes> encode_posting_entry(
    const PostingEntry& entry) {
    std::array<std::uint8_t, kPostingEntryBytes> bytes{};
    put_le(&bytes, 0U, entry.node_ordinal);
    put_le(&bytes, 8U, entry.next_posting);
    put_le(&bytes, 16U, entry.source_record_index);
    put_le(&bytes, 24U, entry.record_byte_offset);
    put_le(&bytes, 32U, entry.record_byte_length);
    put_le(&bytes, 40U, std::uint32_t{0U});
    put_le(
        &bytes,
        44U,
        crc32(std::span<const std::uint8_t>(bytes.data(), 44U)));
    return bytes;
}

bool decode_posting_entry(
    std::span<const std::uint8_t> bytes,
    PostingEntry* entry,
    std::string* error) {
    if (entry == nullptr || bytes.size() != kPostingEntryBytes) {
        return fail(
            error,
            "logical-node record-index posting entry has invalid size");
    }
    if (get_le<std::uint32_t>(bytes, 40U) != 0U ||
        crc32(bytes.first(44U)) != get_le<std::uint32_t>(bytes, 44U)) {
        return fail(
            error,
            "logical-node record-index posting entry CRC/reserved mismatch");
    }
    entry->node_ordinal = get_le<std::uint64_t>(bytes, 0U);
    entry->next_posting = get_le<std::uint64_t>(bytes, 8U);
    entry->source_record_index = get_le<std::uint64_t>(bytes, 16U);
    entry->record_byte_offset = get_le<std::uint64_t>(bytes, 24U);
    entry->record_byte_length = get_le<std::uint64_t>(bytes, 32U);
    if (entry->record_byte_length == 0U) {
        return fail(
            error,
            "logical-node record-index posting has zero overlap length");
    }
    return true;
}

std::array<std::uint8_t, kManifestBytes> encode_manifest(
    const LogicalNodeRecordIndexManifest& manifest) {
    std::array<std::uint8_t, kManifestBytes> bytes{};
    std::copy(kManifestMagic.begin(), kManifestMagic.end(), bytes.begin());
    put_le(&bytes, 8U, manifest.format_version);
    put_le(&bytes, 12U, static_cast<std::uint32_t>(kManifestBytes));
    put_le(&bytes, 16U, manifest.source_record_count);
    put_le(&bytes, 24U, manifest.node_count);
    put_le(&bytes, 32U, manifest.posting_count);
    put_le(&bytes, 40U, manifest.total_source_bytes);
    std::copy(
        manifest.source_binding.payload_sha256.begin(),
        manifest.source_binding.payload_sha256.end(),
        bytes.begin() + 48);
    std::copy(
        manifest.source_binding.record_sequence_sha256.begin(),
        manifest.source_binding.record_sequence_sha256.end(),
        bytes.begin() + 80);
    std::copy(
        manifest.arena_identity_sha256.begin(),
        manifest.arena_identity_sha256.end(),
        bytes.begin() + 112);
    put_le(&bytes, 144U, std::uint32_t{0U});
    put_le(
        &bytes,
        148U,
        crc32(std::span<const std::uint8_t>(bytes.data(), 148U)));
    return bytes;
}

bool decode_manifest(
    std::span<const std::uint8_t> bytes,
    LogicalNodeRecordIndexManifest* manifest,
    std::string* error) {
    if (manifest == nullptr || bytes.size() != kManifestBytes) {
        return fail(
            error,
            "logical-node record-index manifest has invalid size");
    }
    if (!std::equal(
            kManifestMagic.begin(),
            kManifestMagic.end(),
            bytes.begin()) ||
        get_le<std::uint32_t>(bytes, 8U) !=
            kLogicalNodeRecordIndexFormatVersion ||
        get_le<std::uint32_t>(bytes, 12U) != kManifestBytes ||
        get_le<std::uint32_t>(bytes, 144U) != 0U ||
        crc32(bytes.first(148U)) != get_le<std::uint32_t>(bytes, 148U)) {
        return fail(
            error,
            "logical-node record-index manifest magic/version/CRC mismatch");
    }

    LogicalNodeRecordIndexManifest parsed;
    parsed.format_version = get_le<std::uint32_t>(bytes, 8U);
    parsed.source_record_count = get_le<std::uint64_t>(bytes, 16U);
    parsed.node_count = get_le<std::uint64_t>(bytes, 24U);
    parsed.posting_count = get_le<std::uint64_t>(bytes, 32U);
    parsed.total_source_bytes = get_le<std::uint64_t>(bytes, 40U);
    parsed.source_binding.source_record_count = parsed.source_record_count;
    std::copy_n(
        bytes.begin() + 48,
        32U,
        parsed.source_binding.payload_sha256.begin());
    std::copy_n(
        bytes.begin() + 80,
        32U,
        parsed.source_binding.record_sequence_sha256.begin());
    std::copy_n(
        bytes.begin() + 112,
        32U,
        parsed.arena_identity_sha256.begin());
    *manifest = parsed;
    return true;
}

bool write_exact(
    std::ofstream* stream,
    std::span<const std::byte> bytes,
    std::string* error) {
    if (stream == nullptr || !*stream) {
        return fail(
            error,
            "logical-node record-index output stream is not writable");
    }
    if (bytes.size() >
        static_cast<std::size_t>(
            std::numeric_limits<std::streamsize>::max())) {
        return fail(
            error,
            "logical-node record-index output transfer exceeds streamsize");
    }
    stream->write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!*stream) {
        return fail(
            error,
            "cannot write logical-node record-index staging file");
    }
    return true;
}

class RandomAccessFile final {
public:
    explicit RandomAccessFile(std::filesystem::path path)
        : path_(std::move(path)) {}

    RandomAccessFile(const RandomAccessFile&) = delete;
    RandomAccessFile& operator=(const RandomAccessFile&) = delete;

    bool open(std::string* error) {
        stream_.open(
            path_,
            std::ios::binary | std::ios::in | std::ios::out);
        if (!stream_) {
            return fail(
                error,
                "cannot open logical-node record-index random-access file");
        }
        return true;
    }

    bool read_exact_at(
        std::uint64_t offset,
        std::span<std::uint8_t> output,
        std::string* error) {
        if (!stream_) {
            return fail(
                error,
                "logical-node record-index random-access file is not open");
        }
        if (offset > static_cast<std::uint64_t>(
                         std::numeric_limits<std::streamoff>::max()) ||
            output.size() > static_cast<std::size_t>(
                                std::numeric_limits<std::streamsize>::max())) {
            return fail(
                error,
                "logical-node record-index random-access read exceeds stream range");
        }
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!stream_) {
            return fail(
                error,
                "cannot seek logical-node record-index random-access reader");
        }
        stream_.read(
            reinterpret_cast<char*>(output.data()),
            static_cast<std::streamsize>(output.size()));
        if (stream_.gcount() != static_cast<std::streamsize>(output.size())) {
            return fail(
                error,
                "unexpected EOF in logical-node record-index random-access file");
        }
        return true;
    }

    bool write_exact_at(
        std::uint64_t offset,
        std::span<const std::uint8_t> input,
        std::string* error) {
        if (!stream_) {
            return fail(
                error,
                "logical-node record-index random-access file is not open");
        }
        if (offset > static_cast<std::uint64_t>(
                         std::numeric_limits<std::streamoff>::max()) ||
            input.size() > static_cast<std::size_t>(
                               std::numeric_limits<std::streamsize>::max())) {
            return fail(
                error,
                "logical-node record-index random-access write exceeds stream range");
        }
        stream_.clear();
        stream_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!stream_) {
            return fail(
                error,
                "cannot seek logical-node record-index random-access writer");
        }
        stream_.write(
            reinterpret_cast<const char*>(input.data()),
            static_cast<std::streamsize>(input.size()));
        if (!stream_) {
            return fail(
                error,
                "cannot write logical-node record-index random-access file");
        }
        return true;
    }

    bool flush(std::string* error) {
        stream_.flush();
        if (!stream_) {
            return fail(
                error,
                "cannot flush logical-node record-index random-access file");
        }
        return true;
    }

    void close() noexcept {
        if (stream_.is_open()) {
            stream_.close();
        }
    }

private:
    std::filesystem::path path_;
    std::fstream stream_;
};

template <std::size_t N>
bool read_fixed_at(
    const BoundedPositionalReader& reader,
    std::uint64_t offset,
    std::array<std::uint8_t, N>* bytes,
    std::string* error) {
    if (bytes == nullptr) {
        return false;
    }
    return reader.read_exact_at(offset, as_writable_bytes(bytes), error);
}

bool read_record_entry_at(
    const BoundedPositionalReader& reader,
    std::uint64_t record_index,
    RecordEntry* entry,
    std::string* error) {
    std::uint64_t offset = 0U;
    if (!checked_mul(record_index, kRecordEntryBytes, &offset, error)) {
        return false;
    }
    std::array<std::uint8_t, kRecordEntryBytes> bytes{};
    return read_fixed_at(reader, offset, &bytes, error) &&
        decode_record_entry(bytes, entry, error);
}

bool read_head_entry_at(
    const BoundedPositionalReader& reader,
    std::uint64_t record_index,
    HeadEntry* entry,
    std::string* error) {
    std::uint64_t offset = 0U;
    if (!checked_mul(record_index, kHeadEntryBytes, &offset, error)) {
        return false;
    }
    std::array<std::uint8_t, kHeadEntryBytes> bytes{};
    return read_fixed_at(reader, offset, &bytes, error) &&
        decode_head_entry(bytes, entry, error);
}

bool read_posting_entry_at(
    const BoundedPositionalReader& reader,
    std::uint64_t posting_ordinal,
    PostingEntry* entry,
    std::string* error) {
    std::uint64_t offset = 0U;
    if (!checked_mul(posting_ordinal, kPostingEntryBytes, &offset, error)) {
        return false;
    }
    std::array<std::uint8_t, kPostingEntryBytes> bytes{};
    return read_fixed_at(reader, offset, &bytes, error) &&
        decode_posting_entry(bytes, entry, error);
}

bool read_head_entry_rw(
    RandomAccessFile* file,
    std::uint64_t record_index,
    HeadEntry* entry,
    std::string* error) {
    std::uint64_t offset = 0U;
    if (file == nullptr ||
        !checked_mul(record_index, kHeadEntryBytes, &offset, error)) {
        return false;
    }
    std::array<std::uint8_t, kHeadEntryBytes> bytes{};
    return file->read_exact_at(offset, bytes, error) &&
        decode_head_entry(bytes, entry, error);
}

bool write_head_entry_rw(
    RandomAccessFile* file,
    std::uint64_t record_index,
    const HeadEntry& entry,
    std::string* error) {
    std::uint64_t offset = 0U;
    if (file == nullptr ||
        !checked_mul(record_index, kHeadEntryBytes, &offset, error)) {
        return false;
    }
    const auto bytes = encode_head_entry(entry);
    return file->write_exact_at(offset, bytes, error);
}

bool read_posting_entry_rw(
    RandomAccessFile* file,
    std::uint64_t posting_ordinal,
    PostingEntry* entry,
    std::string* error) {
    std::uint64_t offset = 0U;
    if (file == nullptr ||
        !checked_mul(posting_ordinal, kPostingEntryBytes, &offset, error)) {
        return false;
    }
    std::array<std::uint8_t, kPostingEntryBytes> bytes{};
    return file->read_exact_at(offset, bytes, error) &&
        decode_posting_entry(bytes, entry, error);
}

bool write_posting_entry_rw(
    RandomAccessFile* file,
    std::uint64_t posting_ordinal,
    const PostingEntry& entry,
    std::string* error) {
    std::uint64_t offset = 0U;
    if (file == nullptr ||
        !checked_mul(posting_ordinal, kPostingEntryBytes, &offset, error)) {
        return false;
    }
    const auto bytes = encode_posting_entry(entry);
    return file->write_exact_at(offset, bytes, error);
}

struct StagingCleanup {
    explicit StagingCleanup(std::filesystem::path value)
        : path(std::move(value)) {}
    ~StagingCleanup() {
        if (!published) {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }
    std::filesystem::path path;
    bool published{false};
};

bool create_empty_file(
    const std::filesystem::path& path,
    std::string* error) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return fail(
            error,
            "cannot create logical-node record-index staging file");
    }
    return true;
}

bool validate_expected_file_size(
    const BoundedPositionalReader& reader,
    std::uint64_t count,
    std::size_t entry_bytes,
    std::string_view label,
    std::string* error) {
    std::uint64_t expected = 0U;
    if (!checked_mul(count, entry_bytes, &expected, error)) {
        return false;
    }
    if (reader.file_size() != expected) {
        return fail(
            error,
            "logical-node record-index " + std::string(label) +
                " file size does not match manifest");
    }
    return true;
}

bool validate_posting_overlap(
    const BoundedPositionalReader& records,
    const LogicalNodeArenaV2StoreBoundReader& arena,
    const LogicalNodeRecordIndexManifest& manifest,
    std::uint64_t source_record_index,
    const PostingEntry& posting,
    std::string* error) {
    if (posting.source_record_index != source_record_index) {
        return fail(
            error,
            "logical-node record-index posting belongs to a different source record");
    }
    if (posting.node_ordinal >= manifest.node_count) {
        return fail(
            error,
            "logical-node record-index posting node ordinal is out of range");
    }

    LogicalNodeRecord node;
    if (!arena.node_by_ordinal(posting.node_ordinal, &node, error)) {
        return false;
    }
    if (node.source_byte_length == 0U ||
        node.source_record_index >= manifest.source_record_count) {
        return fail(
            error,
            "logical-node record-index posting references invalid node source span");
    }

    RecordEntry node_start_record;
    RecordEntry target_record;
    if (!read_record_entry_at(
            records, node.source_record_index, &node_start_record, error) ||
        !read_record_entry_at(
            records, source_record_index, &target_record, error)) {
        return false;
    }
    if (node.source_byte_offset > node_start_record.byte_length) {
        return fail(
            error,
            "logical-node source offset exceeds indexed start record");
    }

    std::uint64_t node_start = 0U;
    std::uint64_t node_end = 0U;
    std::uint64_t target_end = 0U;
    if (!checked_add(
            node_start_record.absolute_start,
            node.source_byte_offset,
            &node_start,
            error) ||
        !checked_add(node_start, node.source_byte_length, &node_end, error) ||
        !checked_add(
            target_record.absolute_start,
            target_record.byte_length,
            &target_end,
            error)) {
        return false;
    }
    if (node_end > manifest.total_source_bytes) {
        return fail(
            error,
            "logical-node source span exceeds indexed source bytes");
    }

    const std::uint64_t overlap_start =
        std::max(node_start, target_record.absolute_start);
    const std::uint64_t overlap_end = std::min(node_end, target_end);
    if (overlap_start >= overlap_end) {
        return fail(
            error,
            "logical-node record-index posting does not overlap queried record");
    }
    const std::uint64_t expected_offset =
        overlap_start - target_record.absolute_start;
    const std::uint64_t expected_length = overlap_end - overlap_start;
    if (posting.record_byte_offset != expected_offset ||
        posting.record_byte_length != expected_length) {
        return fail(
            error,
            "logical-node record-index posting overlap payload is inconsistent");
    }
    return true;
}

} // namespace

bool build_logical_node_record_index(
    const std::filesystem::path& store_root,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();

    const std::filesystem::path final_dir =
        store_root / "node-record-index-v1";
    const std::filesystem::path staging =
        store_root / "node-record-index-v1.building";
    if (std::filesystem::exists(final_dir)) {
        return fail(error, "logical-node record-index already exists");
    }
    {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
    }

    LogicalNodeSourceStoreBinding binding;
    if (!inspect_logical_node_source_store_binding(
            store_root, &binding, error)) {
        return false;
    }
    StoreReader store(store_root);
    if (!store.open(error)) {
        return false;
    }
    LogicalNodeArenaV2StoreBoundReader arena(store_root);
    if (!arena.open(error)) {
        return false;
    }
    if (!source_binding_equal(binding, arena.source_binding()) ||
        binding.source_record_count != store.stats().corpus.logical_records) {
        return fail(
            error,
            "logical-node record-index store/arena binding mismatch");
    }

    LogicalNodeRecordIndexManifest manifest;
    manifest.format_version = kLogicalNodeRecordIndexFormatVersion;
    manifest.source_record_count = binding.source_record_count;
    manifest.node_count = arena.manifest().storage_manifest.node_count;
    manifest.total_source_bytes = store.stats().corpus.logical_utf8_bytes;
    manifest.source_binding = binding;
    if (!compute_arena_identity(
            arena.manifest(), &manifest.arena_identity_sha256, error)) {
        return false;
    }

    std::uint64_t expected_records_index_bytes = 0U;
    if (!checked_mul(
            manifest.source_record_count,
            kStoreRecordDescriptorBytes,
            &expected_records_index_bytes,
            error)) {
        return false;
    }
    BoundedPositionalReader store_records(
        store_root / "records.idx",
        kIoWindowBytes);
    if (!store_records.open(error) ||
        store_records.file_size() != expected_records_index_bytes) {
        if (error->empty()) {
            *error =
                "records.idx size changed while building logical-node record-index";
        }
        return false;
    }

    if (!std::filesystem::create_directory(staging)) {
        return fail(
            error,
            "cannot create logical-node record-index staging directory");
    }
    StagingCleanup cleanup(staging);

    const std::filesystem::path records_path = staging / "records.bin";
    const std::filesystem::path heads_path = staging / "heads.bin";
    const std::filesystem::path postings_path = staging / "postings.bin";
    {
        std::ofstream records_out(
            records_path,
            std::ios::binary | std::ios::trunc);
        std::ofstream heads_out(
            heads_path,
            std::ios::binary | std::ios::trunc);
        if (!records_out || !heads_out) {
            return fail(
                error,
                "cannot create logical-node record-index record/head tables");
        }

        const std::size_t descriptors_per_batch = std::max<std::size_t>(
            1U,
            kIoWindowBytes / kStoreRecordDescriptorBytes);
        std::vector<std::byte> raw(
            descriptors_per_batch * kStoreRecordDescriptorBytes);
        std::uint64_t prefix = 0U;
        std::uint64_t first = 0U;
        while (first < manifest.source_record_count) {
            const std::uint64_t remaining =
                manifest.source_record_count - first;
            const std::size_t count = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, descriptors_per_batch));
            const std::size_t byte_count =
                count * kStoreRecordDescriptorBytes;
            std::span<std::byte> batch(raw.data(), byte_count);
            if (!store_records.read_exact_at(
                    first * static_cast<std::uint64_t>(
                                kStoreRecordDescriptorBytes),
                    batch,
                    error)) {
                return false;
            }

            const auto descriptor = std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(raw.data()),
                byte_count);
            for (std::size_t relative = 0U; relative < count; ++relative) {
                const std::size_t base =
                    relative * kStoreRecordDescriptorBytes +
                    kStoreRecordLengthOffset;
                const std::uint64_t length =
                    get_le<std::uint64_t>(descriptor, base);
                const auto record_bytes =
                    encode_record_entry(RecordEntry{prefix, length});
                const auto head_bytes = encode_head_entry(HeadEntry{});
                if (!write_exact(
                        &records_out, as_bytes(record_bytes), error) ||
                    !write_exact(
                        &heads_out, as_bytes(head_bytes), error) ||
                    !checked_add(prefix, length, &prefix, error)) {
                    return false;
                }
            }
            first += static_cast<std::uint64_t>(count);
        }
        if (prefix != manifest.total_source_bytes) {
            return fail(
                error,
                "logical-node record-index record lengths disagree with store logical bytes");
        }
        records_out.flush();
        heads_out.flush();
        if (!records_out || !heads_out) {
            return fail(
                error,
                "cannot flush logical-node record-index record/head tables");
        }
    }
    if (!create_empty_file(postings_path, error)) {
        return false;
    }

    BoundedPositionalReader record_table(records_path, kIoWindowBytes);
    if (!record_table.open(error)) {
        return false;
    }

    std::uint64_t posting_count = 0U;
    {
        RandomAccessFile heads(heads_path);
        RandomAccessFile postings(postings_path);
        if (!heads.open(error) || !postings.open(error)) {
            return false;
        }

        for (std::uint64_t node_ordinal = 0U;
             node_ordinal < manifest.node_count;
             ++node_ordinal) {
            LogicalNodeRecord node;
            if (!arena.node_by_ordinal(node_ordinal, &node, error)) {
                return false;
            }
            if (node.source_byte_length == 0U) {
                continue;
            }
            if (node.source_record_index >= manifest.source_record_count) {
                return fail(
                    error,
                    "logical-node source record is outside record-index domain");
            }

            std::uint64_t record_index = node.source_record_index;
            std::uint64_t local_offset = node.source_byte_offset;
            std::uint64_t remaining = node.source_byte_length;
            while (remaining != 0U) {
                RecordEntry record;
                if (!read_record_entry_at(
                        record_table, record_index, &record, error)) {
                    return false;
                }
                if (local_offset > record.byte_length) {
                    return fail(
                        error,
                        "logical-node source offset exceeds physical record length");
                }
                const std::uint64_t available =
                    record.byte_length - local_offset;
                const std::uint64_t overlap =
                    std::min(remaining, available);
                if (overlap != 0U) {
                    if (posting_count ==
                        std::numeric_limits<std::uint64_t>::max()) {
                        return fail(
                            error,
                            "logical-node record-index posting count overflows");
                    }

                    HeadEntry head;
                    if (!read_head_entry_rw(
                            &heads, record_index, &head, error)) {
                        return false;
                    }
                    if (head.posting_count ==
                        std::numeric_limits<std::uint64_t>::max()) {
                        return fail(
                            error,
                            "logical-node record-index per-record posting count overflows");
                    }

                    const PostingEntry appended{
                        node_ordinal,
                        kNoLogicalNodeRecordPosting,
                        record_index,
                        local_offset,
                        overlap};
                    if (!write_posting_entry_rw(
                            &postings,
                            posting_count,
                            appended,
                            error)) {
                        return false;
                    }

                    if (head.posting_count == 0U) {
                        head.first_posting = posting_count;
                        head.last_posting = posting_count;
                    } else {
                        if (head.last_posting >= posting_count) {
                            return fail(
                                error,
                                "logical-node record-index tail ordering is invalid");
                        }
                        PostingEntry tail;
                        if (!read_posting_entry_rw(
                                &postings,
                                head.last_posting,
                                &tail,
                                error) ||
                            tail.source_record_index != record_index ||
                            tail.next_posting !=
                                kNoLogicalNodeRecordPosting) {
                            if (error->empty()) {
                                *error =
                                    "logical-node record-index tail identity/link is invalid";
                            }
                            return false;
                        }
                        tail.next_posting = posting_count;
                        if (!write_posting_entry_rw(
                                &postings,
                                head.last_posting,
                                tail,
                                error)) {
                            return false;
                        }
                        head.last_posting = posting_count;
                    }
                    ++head.posting_count;
                    if (!write_head_entry_rw(
                            &heads, record_index, head, error)) {
                        return false;
                    }
                    ++posting_count;
                    remaining -= overlap;
                }

                if (remaining == 0U) {
                    break;
                }
                if (record_index + 1U >= manifest.source_record_count) {
                    return fail(
                        error,
                        "logical-node source span escapes physical record sequence");
                }
                ++record_index;
                local_offset = 0U;
            }
        }
        if (!heads.flush(error) || !postings.flush(error)) {
            return false;
        }
        // Close staging-file handles before directory publication. This is
        // required on Windows, where open non-delete-sharing handles can block
        // the final directory rename.
        heads.close();
        postings.close();
    }

    manifest.posting_count = posting_count;
    const auto manifest_bytes = encode_manifest(manifest);
    {
        std::ofstream manifest_out(
            staging / "manifest.bin",
            std::ios::binary | std::ios::trunc);
        if (!manifest_out ||
            !write_exact(&manifest_out, as_bytes(manifest_bytes), error)) {
            return false;
        }
        manifest_out.flush();
        if (!manifest_out) {
            return fail(
                error,
                "cannot flush logical-node record-index manifest");
        }
    }

    std::error_code rename_error;
    std::filesystem::rename(staging, final_dir, rename_error);
    if (rename_error) {
        return fail(
            error,
            "cannot publish logical-node record-index: " +
                rename_error.message());
    }
    cleanup.published = true;
    return true;
}

struct LogicalNodeRecordIndexReader::Impl {
    explicit Impl(std::filesystem::path value)
        : store_root(std::move(value)) {}

    std::filesystem::path store_root;
    LogicalNodeRecordIndexManifest manifest{};
    std::unique_ptr<BoundedPositionalReader> records;
    std::unique_ptr<BoundedPositionalReader> heads;
    std::unique_ptr<BoundedPositionalReader> postings;
    std::unique_ptr<LogicalNodeArenaV2StoreBoundReader> arena;
    bool opened{false};
};

LogicalNodeRecordIndexReader::LogicalNodeRecordIndexReader(
    std::filesystem::path store_root)
    : impl_(std::make_unique<Impl>(std::move(store_root))) {}

LogicalNodeRecordIndexReader::~LogicalNodeRecordIndexReader() = default;
LogicalNodeRecordIndexReader::LogicalNodeRecordIndexReader(
    LogicalNodeRecordIndexReader&&) noexcept = default;
LogicalNodeRecordIndexReader& LogicalNodeRecordIndexReader::operator=(
    LogicalNodeRecordIndexReader&&) noexcept = default;

bool LogicalNodeRecordIndexReader::open(std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (impl_->opened) {
        return fail(
            error,
            "logical-node record-index reader is already open");
    }

    const std::filesystem::path root =
        impl_->store_root / "node-record-index-v1";
    BoundedPositionalReader manifest_reader(
        root / "manifest.bin",
        kManifestBytes);
    if (!manifest_reader.open(error) ||
        manifest_reader.file_size() != kManifestBytes) {
        if (error->empty()) {
            *error =
                "logical-node record-index manifest has invalid file size";
        }
        return false;
    }
    std::array<std::uint8_t, kManifestBytes> manifest_bytes{};
    if (!read_fixed_at(
            manifest_reader, 0U, &manifest_bytes, error) ||
        !decode_manifest(
            manifest_bytes, &impl_->manifest, error)) {
        return false;
    }

    LogicalNodeSourceStoreBinding current_binding;
    if (!inspect_logical_node_source_store_binding(
            impl_->store_root, &current_binding, error) ||
        !source_binding_equal(
            current_binding, impl_->manifest.source_binding)) {
        if (error->empty()) {
            *error =
                "logical-node record-index native-store binding mismatch";
        }
        return false;
    }

    StoreReader store(impl_->store_root);
    if (!store.open(error) ||
        store.stats().corpus.logical_utf8_bytes !=
            impl_->manifest.total_source_bytes) {
        if (error->empty()) {
            *error =
                "logical-node record-index total source-byte mismatch";
        }
        return false;
    }

    impl_->arena =
        std::make_unique<LogicalNodeArenaV2StoreBoundReader>(
            impl_->store_root);
    if (!impl_->arena->open(error) ||
        !source_binding_equal(
            impl_->arena->source_binding(),
            impl_->manifest.source_binding) ||
        impl_->arena->manifest().storage_manifest.node_count !=
            impl_->manifest.node_count) {
        if (error->empty()) {
            *error =
                "logical-node record-index arena binding/count mismatch";
        }
        return false;
    }
    std::array<std::uint8_t, 32> current_arena_identity{};
    if (!compute_arena_identity(
            impl_->arena->manifest(),
            &current_arena_identity,
            error) ||
        current_arena_identity !=
            impl_->manifest.arena_identity_sha256) {
        if (error->empty()) {
            *error =
                "logical-node record-index arena identity mismatch";
        }
        return false;
    }

    impl_->records = std::make_unique<BoundedPositionalReader>(
        root / "records.bin", kIoWindowBytes);
    impl_->heads = std::make_unique<BoundedPositionalReader>(
        root / "heads.bin", kIoWindowBytes);
    impl_->postings = std::make_unique<BoundedPositionalReader>(
        root / "postings.bin", kIoWindowBytes);
    if (!impl_->records->open(error) ||
        !impl_->heads->open(error) ||
        !impl_->postings->open(error) ||
        !validate_expected_file_size(
            *impl_->records,
            impl_->manifest.source_record_count,
            kRecordEntryBytes,
            "record",
            error) ||
        !validate_expected_file_size(
            *impl_->heads,
            impl_->manifest.source_record_count,
            kHeadEntryBytes,
            "head",
            error) ||
        !validate_expected_file_size(
            *impl_->postings,
            impl_->manifest.posting_count,
            kPostingEntryBytes,
            "posting",
            error)) {
        return false;
    }

    impl_->opened = true;
    return true;
}

const LogicalNodeRecordIndexManifest&
LogicalNodeRecordIndexReader::manifest() const noexcept {
    return impl_->manifest;
}

bool LogicalNodeRecordIndexReader::read_record(
    std::uint64_t source_record_index,
    std::uint64_t continuation_posting_ordinal,
    std::size_t max_nodes,
    LogicalNodeRecordIndexWindow* result,
    std::string* error) const {
    if (result == nullptr || error == nullptr) {
        return false;
    }
    *result = LogicalNodeRecordIndexWindow{};
    error->clear();
    if (!impl_->opened) {
        return fail(
            error,
            "logical-node record-index reader is not open");
    }
    if (source_record_index >= impl_->manifest.source_record_count) {
        return fail(
            error,
            "logical-node record-index source record is out of range");
    }
    if (max_nodes == 0U ||
        max_nodes > kMaximumLogicalNodeRecordWindowNodes) {
        return fail(
            error,
            "logical-node record-index max_nodes is outside supported bounds");
    }

    HeadEntry head;
    if (!read_head_entry_at(
            *impl_->heads, source_record_index, &head, error)) {
        return false;
    }
    if (head.first_posting != kNoLogicalNodeRecordPosting &&
        head.first_posting >= impl_->manifest.posting_count) {
        return fail(
            error,
            "logical-node record-index head first posting is out of range");
    }
    if (head.last_posting != kNoLogicalNodeRecordPosting &&
        head.last_posting >= impl_->manifest.posting_count) {
        return fail(
            error,
            "logical-node record-index head last posting is out of range");
    }

    std::uint64_t current = continuation_posting_ordinal;
    std::uint64_t expected_remaining = kNoLogicalNodeRecordPosting;
    if (current == kNoLogicalNodeRecordPosting) {
        current = head.first_posting;
        expected_remaining = head.posting_count;
    } else if (current >= impl_->manifest.posting_count) {
        return fail(
            error,
            "logical-node record-index continuation is out of range");
    }

    result->postings.reserve(
        std::min<std::size_t>(max_nodes, 256U));
    std::uint64_t emitted = 0U;
    while (current != kNoLogicalNodeRecordPosting &&
           result->postings.size() < max_nodes) {
        if (current >= impl_->manifest.posting_count) {
            return fail(
                error,
                "logical-node record-index posting link is out of range");
        }
        PostingEntry posting;
        if (!read_posting_entry_at(
                *impl_->postings, current, &posting, error) ||
            !validate_posting_overlap(
                *impl_->records,
                *impl_->arena,
                impl_->manifest,
                source_record_index,
                posting,
                error)) {
            return false;
        }
        if (posting.next_posting != kNoLogicalNodeRecordPosting &&
            posting.next_posting <= current) {
            return fail(
                error,
                "logical-node record-index posting link is not strictly forward");
        }

        result->postings.push_back(LogicalNodeRecordPosting{
            current,
            posting.node_ordinal,
            posting.record_byte_offset,
            posting.record_byte_length});
        ++emitted;
        current = posting.next_posting;
    }

    if (expected_remaining != kNoLogicalNodeRecordPosting) {
        if (emitted > expected_remaining) {
            return fail(
                error,
                "logical-node record-index head count underflows");
        }
        if (current == kNoLogicalNodeRecordPosting &&
            emitted != expected_remaining) {
            return fail(
                error,
                "logical-node record-index head count disagrees with posting chain");
        }
        if (current != kNoLogicalNodeRecordPosting &&
            emitted == expected_remaining) {
            return fail(
                error,
                "logical-node record-index posting chain exceeds head count");
        }
    }

    result->next_posting_ordinal = current;
    result->truncated = current != kNoLogicalNodeRecordPosting;
    return true;
}

} // namespace zevryon::massivedoc
