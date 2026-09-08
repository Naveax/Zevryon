#include "logical_node_source_record_index.hpp"

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
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zevryon::massivedoc {
namespace {

constexpr std::array<std::uint8_t, 8> kManifestMagic{
    'Z', 'V', 'N', 'R', 'I', 'D', 'X', '1'};
constexpr std::uint32_t kManifestBytes = 160U;
constexpr std::uint64_t kHeadBytes = 32U;
constexpr std::uint64_t kPostingBytes = 56U;
constexpr std::uint64_t kRecordDescriptorBytes = 32U;
constexpr std::string_view kArenaIdentityDomain =
    "ZEVRYON-ZVNRIDX1-ARENA-IDENTITY";

struct HeadRecord {
    std::uint64_t first{kNoLogicalNodeSourceRecordPosting};
    std::uint64_t last{kNoLogicalNodeSourceRecordPosting};
    std::uint64_t count{0U};
};

struct PostingRecord {
    std::uint64_t node_ordinal{0U};
    std::uint64_t source_record_index{0U};
    std::uint64_t source_byte_offset{0U};
    std::uint64_t source_byte_length{0U};
    std::uint64_t node_span_offset{0U};
    std::uint64_t next{kNoLogicalNodeSourceRecordPosting};
};

bool fail_index(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool checked_multiply(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t* output) noexcept {
    if (output == nullptr ||
        (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left)) {
        return false;
    }
    *output = left * right;
    return true;
}

bool checked_add(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t* output) noexcept {
    if (output == nullptr || left > std::numeric_limits<std::uint64_t>::max() - right) {
        return false;
    }
    *output = left + right;
    return true;
}

void write_u32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned index = 0U; index < 4U; ++index) {
        bytes[offset + index] =
            static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
    }
}

void write_u64(std::span<std::uint8_t> bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned index = 0U; index < 8U; ++index) {
        bytes[offset + index] =
            static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
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

std::uint64_t read_u64_bytes(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint64_t value = 0U;
    for (unsigned index = 0U; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<std::uint8_t>(bytes[offset + index]))
                 << (index * 8U);
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

bool same_binding(
    const LogicalNodeSourceStoreBinding& left,
    const LogicalNodeSourceStoreBinding& right) noexcept {
    return left.source_record_count == right.source_record_count &&
        left.payload_sha256 == right.payload_sha256 &&
        left.record_sequence_sha256 == right.record_sequence_sha256;
}

std::filesystem::path final_root(const std::filesystem::path& store_root) {
    return store_root / "node-source-record-index-v1";
}

std::filesystem::path staging_root(const std::filesystem::path& store_root) {
    return store_root / "node-source-record-index-v1.building";
}

std::filesystem::path manifest_path(const std::filesystem::path& root) {
    return root / "manifest.bin";
}

std::filesystem::path heads_path(const std::filesystem::path& root) {
    return root / "heads.bin";
}

std::filesystem::path postings_path(const std::filesystem::path& root) {
    return root / "postings.bin";
}

void remove_tree_noexcept(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
}

bool hash_update_bytes(
    zevryon::text::Sha256* hasher,
    std::span<const std::uint8_t> bytes) {
    return hasher->update(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
}

bool hash_update_string(
    zevryon::text::Sha256* hasher,
    std::string_view text) {
    std::array<std::byte, 8> length{};
    const std::uint64_t size = static_cast<std::uint64_t>(text.size());
    for (unsigned index = 0U; index < 8U; ++index) {
        length[index] = static_cast<std::byte>((size >> (index * 8U)) & 0xffU);
    }
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size());
    return hasher->update(length) && hasher->update(bytes);
}

bool hash_update_u32(zevryon::text::Sha256* hasher, std::uint32_t value) {
    std::array<std::byte, 4> bytes{};
    for (unsigned index = 0U; index < 4U; ++index) {
        bytes[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
    return hasher->update(bytes);
}

bool hash_update_u64(zevryon::text::Sha256* hasher, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (unsigned index = 0U; index < 8U; ++index) {
        bytes[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
    return hasher->update(bytes);
}

bool compute_arena_identity(
    const LogicalNodeArenaV2Manifest& manifest,
    const LogicalNodeSourceStoreBinding& binding,
    std::array<std::uint8_t, 32>* output,
    std::string* error) {
    if (output == nullptr) {
        return fail_index(error, "source-record index arena identity output is null");
    }
    zevryon::text::Sha256 hasher;
    const auto domain = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(kArenaIdentityDomain.data()),
        kArenaIdentityDomain.size());
    if (!hasher.update(domain) ||
        !hash_update_u32(&hasher, manifest.format_version) ||
        !hash_update_u32(&hasher, manifest.storage_manifest.format_version) ||
        !hash_update_u32(&hasher, manifest.storage_manifest.semantic_bucket_count) ||
        !hash_update_u32(&hasher, manifest.storage_manifest.semantic_hash_bits) ||
        !hash_update_u64(&hasher, manifest.storage_manifest.node_count) ||
        !hash_update_u64(&hasher, manifest.storage_manifest.attribute_count)) {
        return fail_index(error, "source-record index arena identity header hash failed");
    }
    for (const std::uint32_t count : manifest.storage_manifest.semantic_counts) {
        if (!hash_update_u32(&hasher, count)) {
            return fail_index(error, "source-record index arena semantic-count hash failed");
        }
    }
    if (!hash_update_string(&hasher, manifest.storage_manifest.candidate_commit) ||
        !hash_update_string(&hasher, manifest.storage_manifest.candidate_tree) ||
        !hash_update_bytes(&hasher, manifest.storage_manifest.source_sha256) ||
        !hash_update_bytes(&hasher, binding.payload_sha256) ||
        !hash_update_bytes(&hasher, binding.record_sequence_sha256) ||
        !hash_update_u64(&hasher, binding.source_record_count)) {
        return fail_index(error, "source-record index arena identity payload hash failed");
    }
    zevryon::text::Sha256Digest digest{};
    if (!hasher.finish(&digest)) {
        return fail_index(error, "source-record index arena identity finish failed");
    }
    for (std::size_t index = 0U; index < output->size(); ++index) {
        (*output)[index] = std::to_integer<std::uint8_t>(digest[index]);
    }
    return true;
}

std::array<std::uint8_t, kHeadBytes> encode_head(const HeadRecord& head) {
    std::array<std::uint8_t, kHeadBytes> bytes{};
    write_u64(bytes, 0U, head.first);
    write_u64(bytes, 8U, head.last);
    write_u64(bytes, 16U, head.count);
    write_u32(bytes, 24U, crc32(std::span<const std::uint8_t>(bytes.data(), 24U)));
    return bytes;
}

bool decode_head(
    std::span<const std::uint8_t> bytes,
    HeadRecord* head,
    std::string* error) {
    if (head == nullptr || bytes.size() != kHeadBytes) {
        return fail_index(error, "source-record index head size mismatch");
    }
    if (read_u32(bytes, 28U) != 0U ||
        crc32(bytes.first(24U)) != read_u32(bytes, 24U)) {
        return fail_index(error, "source-record index head CRC/reserved mismatch");
    }
    HeadRecord decoded;
    decoded.first = read_u64(bytes, 0U);
    decoded.last = read_u64(bytes, 8U);
    decoded.count = read_u64(bytes, 16U);
    if (decoded.count == 0U) {
        if (decoded.first != kNoLogicalNodeSourceRecordPosting ||
            decoded.last != kNoLogicalNodeSourceRecordPosting) {
            return fail_index(error, "empty source-record index head has posting links");
        }
    } else if (decoded.first == kNoLogicalNodeSourceRecordPosting ||
               decoded.last == kNoLogicalNodeSourceRecordPosting ||
               decoded.first > decoded.last) {
        return fail_index(error, "non-empty source-record index head is invalid");
    }
    *head = decoded;
    return true;
}

std::array<std::uint8_t, kPostingBytes> encode_posting(const PostingRecord& posting) {
    std::array<std::uint8_t, kPostingBytes> bytes{};
    write_u64(bytes, 0U, posting.node_ordinal);
    write_u64(bytes, 8U, posting.source_record_index);
    write_u64(bytes, 16U, posting.source_byte_offset);
    write_u64(bytes, 24U, posting.source_byte_length);
    write_u64(bytes, 32U, posting.node_span_offset);
    write_u64(bytes, 40U, posting.next);
    write_u32(bytes, 48U, crc32(std::span<const std::uint8_t>(bytes.data(), 48U)));
    return bytes;
}

bool decode_posting(
    std::span<const std::uint8_t> bytes,
    PostingRecord* posting,
    std::string* error) {
    if (posting == nullptr || bytes.size() != kPostingBytes) {
        return fail_index(error, "source-record index posting size mismatch");
    }
    if (read_u32(bytes, 52U) != 0U ||
        crc32(bytes.first(48U)) != read_u32(bytes, 48U)) {
        return fail_index(error, "source-record index posting CRC/reserved mismatch");
    }
    PostingRecord decoded;
    decoded.node_ordinal = read_u64(bytes, 0U);
    decoded.source_record_index = read_u64(bytes, 8U);
    decoded.source_byte_offset = read_u64(bytes, 16U);
    decoded.source_byte_length = read_u64(bytes, 24U);
    decoded.node_span_offset = read_u64(bytes, 32U);
    decoded.next = read_u64(bytes, 40U);
    if (decoded.source_byte_length == 0U) {
        return fail_index(error, "source-record index posting has zero overlap length");
    }
    *posting = decoded;
    return true;
}

std::array<std::uint8_t, kManifestBytes> encode_manifest(
    const LogicalNodeSourceRecordIndexManifest& manifest,
    std::uint64_t heads_bytes,
    std::uint64_t postings_bytes) {
    std::array<std::uint8_t, kManifestBytes> bytes{};
    std::copy(kManifestMagic.begin(), kManifestMagic.end(), bytes.begin());
    write_u32(bytes, 8U, kLogicalNodeSourceRecordIndexFormatVersion);
    write_u32(bytes, 12U, kManifestBytes);
    write_u64(bytes, 16U, manifest.source_record_count);
    write_u64(bytes, 24U, manifest.node_count);
    write_u64(bytes, 32U, manifest.posting_count);
    std::copy(
        manifest.source_binding.payload_sha256.begin(),
        manifest.source_binding.payload_sha256.end(),
        bytes.begin() + 40U);
    std::copy(
        manifest.source_binding.record_sequence_sha256.begin(),
        manifest.source_binding.record_sequence_sha256.end(),
        bytes.begin() + 72U);
    std::copy(
        manifest.arena_identity_sha256.begin(),
        manifest.arena_identity_sha256.end(),
        bytes.begin() + 104U);
    write_u64(bytes, 136U, heads_bytes);
    write_u64(bytes, 144U, postings_bytes);
    write_u32(bytes, 156U, crc32(std::span<const std::uint8_t>(bytes.data(), 156U)));
    return bytes;
}

bool decode_manifest(
    std::span<const std::uint8_t> bytes,
    LogicalNodeSourceRecordIndexManifest* manifest,
    std::uint64_t* heads_bytes,
    std::uint64_t* postings_bytes,
    std::string* error) {
    if (manifest == nullptr || heads_bytes == nullptr || postings_bytes == nullptr ||
        bytes.size() != kManifestBytes) {
        return fail_index(error, "source-record index manifest size/output mismatch");
    }
    if (!std::equal(kManifestMagic.begin(), kManifestMagic.end(), bytes.begin()) ||
        read_u32(bytes, 8U) != kLogicalNodeSourceRecordIndexFormatVersion ||
        read_u32(bytes, 12U) != kManifestBytes ||
        read_u32(bytes, 152U) != 0U ||
        crc32(bytes.first(156U)) != read_u32(bytes, 156U)) {
        return fail_index(error, "source-record index manifest format/CRC mismatch");
    }
    LogicalNodeSourceRecordIndexManifest decoded;
    decoded.format_version = read_u32(bytes, 8U);
    decoded.source_record_count = read_u64(bytes, 16U);
    decoded.node_count = read_u64(bytes, 24U);
    decoded.posting_count = read_u64(bytes, 32U);
    std::copy_n(bytes.begin() + 40U, 32U, decoded.source_binding.payload_sha256.begin());
    std::copy_n(
        bytes.begin() + 72U,
        32U,
        decoded.source_binding.record_sequence_sha256.begin());
    std::copy_n(bytes.begin() + 104U, 32U, decoded.arena_identity_sha256.begin());
    decoded.source_binding.source_record_count = decoded.source_record_count;
    *heads_bytes = read_u64(bytes, 136U);
    *postings_bytes = read_u64(bytes, 144U);
    if (decoded.source_record_count == 0U) {
        return fail_index(error, "source-record index manifest has zero records");
    }
    *manifest = decoded;
    return true;
}

bool write_manifest_file(
    const std::filesystem::path& path,
    const LogicalNodeSourceRecordIndexManifest& manifest,
    std::uint64_t heads_bytes,
    std::uint64_t postings_bytes,
    std::string* error) {
    const auto bytes = encode_manifest(manifest, heads_bytes, postings_bytes);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return fail_index(error, "cannot create source-record index manifest");
    }
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    return stream ? true : fail_index(error, "source-record index manifest write failed");
}

bool read_manifest_file(
    const std::filesystem::path& path,
    LogicalNodeSourceRecordIndexManifest* manifest,
    std::uint64_t* heads_bytes,
    std::uint64_t* postings_bytes,
    std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return fail_index(error, "cannot open source-record index manifest");
    }
    std::array<std::uint8_t, kManifestBytes> bytes{};
    stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        return fail_index(error, "source-record index manifest is truncated");
    }
    if (stream.peek() != std::char_traits<char>::eof()) {
        return fail_index(error, "source-record index manifest has trailing bytes");
    }
    return decode_manifest(bytes, manifest, heads_bytes, postings_bytes, error);
}

bool stream_offset(std::uint64_t value, std::streamoff* output, std::string* error) {
    if (output == nullptr || value > static_cast<std::uint64_t>(
            std::numeric_limits<std::streamoff>::max())) {
        return fail_index(error, "source-record index file offset exceeds stream range");
    }
    *output = static_cast<std::streamoff>(value);
    return true;
}

class RecordLengthReader final {
public:
    RecordLengthReader(std::filesystem::path path, std::uint64_t record_count)
        : reader_(std::move(path), kIoWindowBytes), record_count_(record_count) {}

    bool open(std::string* error) {
        if (!reader_.open(error)) {
            return false;
        }
        std::uint64_t expected = 0U;
        if (!checked_multiply(record_count_, kRecordDescriptorBytes, &expected) ||
            reader_.file_size() != expected) {
            return fail_index(error, "source-record index records.idx size mismatch");
        }
        return true;
    }

    bool length(
        std::uint64_t record_index,
        std::uint64_t* output,
        std::string* error) const {
        if (output == nullptr || record_index >= record_count_) {
            return fail_index(error, "source-record index record length lookup is out of range");
        }
        std::uint64_t offset = 0U;
        if (!checked_multiply(record_index, kRecordDescriptorBytes, &offset)) {
            return fail_index(error, "source-record index record descriptor offset overflows");
        }
        std::array<std::byte, kRecordDescriptorBytes> bytes{};
        if (!reader_.read_exact_at(offset, bytes, error)) {
            return false;
        }
        *output = read_u64_bytes(bytes, 16U);
        return true;
    }

private:
    BoundedPositionalReader reader_;
    std::uint64_t record_count_{0U};
};

bool initialize_heads(
    const std::filesystem::path& path,
    std::uint64_t record_count,
    std::string* error) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return fail_index(error, "cannot create source-record index heads file");
    }
    const auto empty = encode_head(HeadRecord{});
    constexpr std::size_t kEntriesPerBatch = kIoWindowBytes / kHeadBytes;
    std::vector<std::uint8_t> batch(kEntriesPerBatch * kHeadBytes);
    for (std::size_t index = 0U; index < kEntriesPerBatch; ++index) {
        std::copy(empty.begin(), empty.end(), batch.begin() + index * kHeadBytes);
    }
    std::uint64_t remaining = record_count;
    while (remaining != 0U) {
        const std::size_t entries = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, kEntriesPerBatch));
        const std::size_t bytes = entries * static_cast<std::size_t>(kHeadBytes);
        stream.write(
            reinterpret_cast<const char*>(batch.data()),
            static_cast<std::streamsize>(bytes));
        if (!stream) {
            return fail_index(error, "source-record index heads initialization failed");
        }
        remaining -= static_cast<std::uint64_t>(entries);
    }
    stream.flush();
    return stream ? true : fail_index(error, "source-record index heads flush failed");
}

bool read_rw_record(
    std::fstream* stream,
    std::uint64_t offset,
    std::span<std::uint8_t> output,
    std::string* error) {
    std::streamoff position = 0;
    if (stream == nullptr || !stream_offset(offset, &position, error)) {
        return false;
    }
    stream->flush();
    stream->clear();
    stream->seekg(position, std::ios::beg);
    if (!*stream) {
        return fail_index(error, "source-record index seekg failed");
    }
    stream->read(
        reinterpret_cast<char*>(output.data()),
        static_cast<std::streamsize>(output.size()));
    return *stream ? true : fail_index(error, "source-record index positional read failed");
}

bool write_rw_record(
    std::fstream* stream,
    std::uint64_t offset,
    std::span<const std::uint8_t> input,
    std::string* error) {
    std::streamoff position = 0;
    if (stream == nullptr || !stream_offset(offset, &position, error)) {
        return false;
    }
    stream->clear();
    stream->seekp(position, std::ios::beg);
    if (!*stream) {
        return fail_index(error, "source-record index seekp failed");
    }
    stream->write(
        reinterpret_cast<const char*>(input.data()),
        static_cast<std::streamsize>(input.size()));
    return *stream ? true : fail_index(error, "source-record index positional write failed");
}

bool append_posting(
    std::fstream* heads,
    std::fstream* postings,
    std::uint64_t source_record_count,
    const PostingRecord& posting,
    std::uint64_t* posting_count,
    std::string* error) {
    if (heads == nullptr || postings == nullptr || posting_count == nullptr ||
        posting.source_record_index >= source_record_count ||
        *posting_count == std::numeric_limits<std::uint64_t>::max()) {
        return fail_index(error, "source-record index posting append input is invalid");
    }
    std::uint64_t head_offset = 0U;
    if (!checked_multiply(posting.source_record_index, kHeadBytes, &head_offset)) {
        return fail_index(error, "source-record index head offset overflows");
    }
    std::array<std::uint8_t, kHeadBytes> head_bytes{};
    if (!read_rw_record(heads, head_offset, head_bytes, error)) {
        return false;
    }
    HeadRecord head;
    if (!decode_head(head_bytes, &head, error)) {
        return false;
    }

    const std::uint64_t new_index = *posting_count;
    PostingRecord new_posting = posting;
    new_posting.next = kNoLogicalNodeSourceRecordPosting;

    if (head.count != 0U) {
        if (head.last >= new_index) {
            return fail_index(error, "source-record index head tail is not append-only");
        }
        std::uint64_t tail_offset = 0U;
        if (!checked_multiply(head.last, kPostingBytes, &tail_offset)) {
            return fail_index(error, "source-record index tail offset overflows");
        }
        std::array<std::uint8_t, kPostingBytes> tail_bytes{};
        if (!read_rw_record(postings, tail_offset, tail_bytes, error)) {
            return false;
        }
        PostingRecord tail;
        if (!decode_posting(tail_bytes, &tail, error) ||
            tail.source_record_index != posting.source_record_index ||
            tail.next != kNoLogicalNodeSourceRecordPosting ||
            tail.node_ordinal >= posting.node_ordinal) {
            return fail_index(error, "source-record index tail violates append ordering");
        }
        tail.next = new_index;
        const auto updated_tail = encode_posting(tail);
        if (!write_rw_record(postings, tail_offset, updated_tail, error)) {
            return false;
        }
    }

    std::uint64_t posting_offset = 0U;
    if (!checked_multiply(new_index, kPostingBytes, &posting_offset)) {
        return fail_index(error, "source-record index posting offset overflows");
    }
    const auto encoded = encode_posting(new_posting);
    if (!write_rw_record(postings, posting_offset, encoded, error)) {
        return false;
    }

    if (head.count == 0U) {
        head.first = new_index;
    }
    head.last = new_index;
    if (head.count == std::numeric_limits<std::uint64_t>::max()) {
        return fail_index(error, "source-record index per-record posting count overflows");
    }
    ++head.count;
    const auto encoded_head = encode_head(head);
    if (!write_rw_record(heads, head_offset, encoded_head, error)) {
        return false;
    }
    ++*posting_count;
    return true;
}

bool read_positional_record(
    const BoundedPositionalReader& reader,
    std::uint64_t index,
    std::uint64_t record_bytes,
    std::span<std::uint8_t> output,
    std::string* error) {
    std::uint64_t offset = 0U;
    if (!checked_multiply(index, record_bytes, &offset)) {
        return fail_index(error, "source-record index positional record offset overflows");
    }
    return reader.read_exact_at(
        offset,
        std::span<std::byte>(reinterpret_cast<std::byte*>(output.data()), output.size()),
        error);
}

} // namespace

bool build_logical_node_source_record_index_v1(
    const std::filesystem::path& store_root,
    LogicalNodeSourceRecordIndexBuildStats* stats,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    LogicalNodeSourceRecordIndexBuildStats local_stats{};
    if (stats != nullptr) {
        *stats = local_stats;
    }

    LogicalNodeArenaV2StoreBoundReader arena(store_root);
    if (!arena.open(error)) {
        return false;
    }
    const LogicalNodeSourceStoreBinding binding = arena.source_binding();
    const std::uint64_t node_count = arena.manifest().storage_manifest.node_count;
    if (binding.source_record_count == 0U) {
        return fail_index(error, "source-record index requires a non-empty store");
    }

    RecordLengthReader record_lengths(store_root / "records.idx", binding.source_record_count);
    if (!record_lengths.open(error)) {
        return false;
    }

    std::array<std::uint8_t, 32> arena_identity{};
    if (!compute_arena_identity(arena.manifest(), binding, &arena_identity, error)) {
        return false;
    }

    const std::filesystem::path final = final_root(store_root);
    const std::filesystem::path staging = staging_root(store_root);
    if (std::filesystem::exists(final) || std::filesystem::exists(staging)) {
        return fail_index(error, "source-record index destination already exists");
    }
    std::error_code create_error;
    if (!std::filesystem::create_directory(staging, create_error) || create_error) {
        return fail_index(error, "cannot create source-record index staging directory");
    }
    struct Cleanup {
        std::filesystem::path path;
        bool published{false};
        ~Cleanup() {
            if (!published) {
                remove_tree_noexcept(path);
            }
        }
    } cleanup{staging, false};

    if (!initialize_heads(heads_path(staging), binding.source_record_count, error)) {
        return false;
    }
    std::fstream heads(
        heads_path(staging),
        std::ios::binary | std::ios::in | std::ios::out);
    std::fstream postings(
        postings_path(staging),
        std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!heads || !postings) {
        return fail_index(error, "cannot open source-record index build files");
    }

    std::uint64_t posting_count = 0U;
    for (std::uint64_t ordinal = 0U; ordinal < node_count; ++ordinal) {
        LogicalNodeRecord node;
        if (!arena.node_by_ordinal(ordinal, &node, error)) {
            return false;
        }
        ++local_stats.nodes_scanned;
        if (node.source_byte_length == 0U) {
            continue;
        }
        if (node.source_record_index >= binding.source_record_count) {
            return fail_index(error, "logical node source record is outside native store");
        }

        std::uint64_t record_index = node.source_record_index;
        std::uint64_t local_offset = node.source_byte_offset;
        std::uint64_t remaining = node.source_byte_length;
        std::uint64_t node_span_offset = 0U;
        std::uint64_t records_for_node = 0U;
        while (remaining != 0U) {
            if (record_index >= binding.source_record_count) {
                return fail_index(error, "logical node source span escapes native store");
            }
            std::uint64_t record_length = 0U;
            if (!record_lengths.length(record_index, &record_length, error)) {
                return false;
            }
            if (local_offset > record_length) {
                return fail_index(error, "logical node source offset escapes physical record");
            }
            const std::uint64_t available = record_length - local_offset;
            if (available == 0U) {
                ++record_index;
                local_offset = 0U;
                continue;
            }
            const std::uint64_t overlap = std::min(remaining, available);
            PostingRecord posting;
            posting.node_ordinal = ordinal;
            posting.source_record_index = record_index;
            posting.source_byte_offset = local_offset;
            posting.source_byte_length = overlap;
            posting.node_span_offset = node_span_offset;
            if (!append_posting(
                    &heads,
                    &postings,
                    binding.source_record_count,
                    posting,
                    &posting_count,
                    error)) {
                return false;
            }
            remaining -= overlap;
            if (!checked_add(node_span_offset, overlap, &node_span_offset)) {
                return fail_index(error, "logical node source span offset overflows");
            }
            ++records_for_node;
            if (remaining != 0U) {
                ++record_index;
                local_offset = 0U;
            }
        }
        ++local_stats.source_nodes_indexed;
        local_stats.maximum_records_per_node =
            std::max(local_stats.maximum_records_per_node, records_for_node);
    }
    heads.flush();
    postings.flush();
    if (!heads || !postings) {
        return fail_index(error, "source-record index build-file flush failed");
    }
    heads.close();
    postings.close();

    std::uint64_t heads_bytes = 0U;
    std::uint64_t postings_bytes = 0U;
    if (!checked_multiply(binding.source_record_count, kHeadBytes, &heads_bytes) ||
        !checked_multiply(posting_count, kPostingBytes, &postings_bytes)) {
        return fail_index(error, "source-record index final file size overflows");
    }
    LogicalNodeSourceRecordIndexManifest manifest;
    manifest.format_version = kLogicalNodeSourceRecordIndexFormatVersion;
    manifest.source_record_count = binding.source_record_count;
    manifest.node_count = node_count;
    manifest.posting_count = posting_count;
    manifest.source_binding = binding;
    manifest.arena_identity_sha256 = arena_identity;
    if (!write_manifest_file(
            manifest_path(staging), manifest, heads_bytes, postings_bytes, error)) {
        return false;
    }

    std::error_code rename_error;
    std::filesystem::rename(staging, final, rename_error);
    if (rename_error) {
        return fail_index(error, "cannot publish source-record index atomically");
    }
    cleanup.published = true;
    local_stats.postings_written = posting_count;
    if (stats != nullptr) {
        *stats = local_stats;
    }
    return true;
}

struct LogicalNodeSourceRecordIndexV1Reader::Impl {
    explicit Impl(std::filesystem::path root_value)
        : store_root(std::move(root_value)), arena(store_root) {}

    std::filesystem::path store_root;
    LogicalNodeArenaV2StoreBoundReader arena;
    LogicalNodeSourceRecordIndexManifest manifest{};
    std::unique_ptr<BoundedPositionalReader> heads;
    std::unique_ptr<BoundedPositionalReader> postings;
    std::unique_ptr<RecordLengthReader> record_lengths;
    bool opened{false};
};

LogicalNodeSourceRecordIndexV1Reader::LogicalNodeSourceRecordIndexV1Reader(
    std::filesystem::path store_root)
    : impl_(std::make_unique<Impl>(std::move(store_root))) {}

LogicalNodeSourceRecordIndexV1Reader::~LogicalNodeSourceRecordIndexV1Reader() = default;
LogicalNodeSourceRecordIndexV1Reader::LogicalNodeSourceRecordIndexV1Reader(
    LogicalNodeSourceRecordIndexV1Reader&&) noexcept = default;
LogicalNodeSourceRecordIndexV1Reader& LogicalNodeSourceRecordIndexV1Reader::operator=(
    LogicalNodeSourceRecordIndexV1Reader&&) noexcept = default;

bool LogicalNodeSourceRecordIndexV1Reader::open(std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (impl_->opened) {
        return fail_index(error, "source-record index reader is already open");
    }
    if (!impl_->arena.open(error)) {
        return false;
    }

    std::uint64_t manifest_heads_bytes = 0U;
    std::uint64_t manifest_postings_bytes = 0U;
    LogicalNodeSourceRecordIndexManifest manifest;
    const std::filesystem::path root = final_root(impl_->store_root);
    if (!read_manifest_file(
            manifest_path(root),
            &manifest,
            &manifest_heads_bytes,
            &manifest_postings_bytes,
            error)) {
        return false;
    }
    if (!same_binding(manifest.source_binding, impl_->arena.source_binding()) ||
        manifest.node_count != impl_->arena.manifest().storage_manifest.node_count) {
        return fail_index(error, "source-record index binding/node count mismatches arena");
    }
    std::array<std::uint8_t, 32> current_identity{};
    if (!compute_arena_identity(
            impl_->arena.manifest(),
            impl_->arena.source_binding(),
            &current_identity,
            error)) {
        return false;
    }
    if (current_identity != manifest.arena_identity_sha256) {
        return fail_index(error, "source-record index arena identity mismatch");
    }

    std::uint64_t expected_heads_bytes = 0U;
    std::uint64_t expected_postings_bytes = 0U;
    if (!checked_multiply(manifest.source_record_count, kHeadBytes, &expected_heads_bytes) ||
        !checked_multiply(manifest.posting_count, kPostingBytes, &expected_postings_bytes) ||
        expected_heads_bytes != manifest_heads_bytes ||
        expected_postings_bytes != manifest_postings_bytes) {
        return fail_index(error, "source-record index manifest file-size arithmetic mismatch");
    }

    auto heads = std::make_unique<BoundedPositionalReader>(
        heads_path(root), kIoWindowBytes);
    auto postings = std::make_unique<BoundedPositionalReader>(
        postings_path(root), kIoWindowBytes);
    auto record_lengths = std::make_unique<RecordLengthReader>(
        impl_->store_root / "records.idx", manifest.source_record_count);
    if (!heads->open(error) || !postings->open(error) || !record_lengths->open(error)) {
        return false;
    }
    if (heads->file_size() != expected_heads_bytes ||
        postings->file_size() != expected_postings_bytes) {
        return fail_index(error, "source-record index physical file size mismatch");
    }

    impl_->manifest = manifest;
    impl_->heads = std::move(heads);
    impl_->postings = std::move(postings);
    impl_->record_lengths = std::move(record_lengths);
    impl_->opened = true;
    return true;
}

const LogicalNodeSourceRecordIndexManifest&
LogicalNodeSourceRecordIndexV1Reader::manifest() const noexcept {
    return impl_->manifest;
}

bool LogicalNodeSourceRecordIndexV1Reader::read_record_page(
    std::uint64_t source_record_index,
    std::uint64_t start_posting_cursor,
    std::size_t max_postings,
    LogicalNodeSourceRecordPage* page,
    std::string* error) const {
    if (page == nullptr || error == nullptr) {
        return false;
    }
    *page = LogicalNodeSourceRecordPage{};
    error->clear();
    if (!impl_->opened || impl_->heads == nullptr || impl_->postings == nullptr ||
        impl_->record_lengths == nullptr) {
        return fail_index(error, "source-record index reader is not open");
    }
    if (source_record_index >= impl_->manifest.source_record_count) {
        return fail_index(error, "source-record index query record is out of range");
    }
    if (max_postings == 0U ||
        max_postings > kMaximumLogicalNodeSourceRecordPagePostings) {
        return fail_index(error, "source-record index page bound is invalid");
    }

    std::array<std::uint8_t, kHeadBytes> head_bytes{};
    if (!read_positional_record(
            *impl_->heads,
            source_record_index,
            kHeadBytes,
            head_bytes,
            error)) {
        return false;
    }
    HeadRecord head;
    if (!decode_head(head_bytes, &head, error) ||
        head.count > impl_->manifest.posting_count) {
        return fail_index(error, "source-record index head posting count is invalid");
    }
    if (head.count != 0U &&
        (head.first >= impl_->manifest.posting_count ||
         head.last >= impl_->manifest.posting_count)) {
        return fail_index(error, "source-record index head posting link is out of range");
    }

    page->source_record_index = source_record_index;
    page->total_postings_for_record = head.count;
    if (head.count == 0U) {
        if (start_posting_cursor != kNoLogicalNodeSourceRecordPosting) {
            return fail_index(error, "empty source-record index record cannot accept a cursor");
        }
        return true;
    }

    std::uint64_t cursor = start_posting_cursor == kNoLogicalNodeSourceRecordPosting
                               ? head.first
                               : start_posting_cursor;
    if (cursor >= impl_->manifest.posting_count) {
        return fail_index(error, "source-record index posting cursor is out of range");
    }
    page->postings.reserve(std::min<std::size_t>(
        max_postings,
        static_cast<std::size_t>(std::min<std::uint64_t>(
            head.count,
            static_cast<std::uint64_t>(max_postings)))));

    std::uint64_t previous_index = kNoLogicalNodeSourceRecordPosting;
    std::uint64_t previous_node = kNoLogicalNodeSourceRecordPosting;
    std::uint64_t last_index = kNoLogicalNodeSourceRecordPosting;
    while (cursor != kNoLogicalNodeSourceRecordPosting &&
           page->postings.size() < max_postings) {
        if (cursor >= impl_->manifest.posting_count ||
            (previous_index != kNoLogicalNodeSourceRecordPosting &&
             cursor <= previous_index)) {
            return fail_index(error, "source-record index posting chain is non-forward/cyclic");
        }
        std::array<std::uint8_t, kPostingBytes> posting_bytes{};
        if (!read_positional_record(
                *impl_->postings,
                cursor,
                kPostingBytes,
                posting_bytes,
                error)) {
            return false;
        }
        PostingRecord posting;
        if (!decode_posting(posting_bytes, &posting, error)) {
            return false;
        }
        if (posting.source_record_index != source_record_index ||
            posting.node_ordinal >= impl_->manifest.node_count ||
            (posting.next != kNoLogicalNodeSourceRecordPosting &&
             (posting.next <= cursor || posting.next >= impl_->manifest.posting_count)) ||
            (previous_node != kNoLogicalNodeSourceRecordPosting &&
             posting.node_ordinal <= previous_node)) {
            return fail_index(error, "source-record index posting identity/order is invalid");
        }

        std::uint64_t record_length = 0U;
        if (!impl_->record_lengths->length(source_record_index, &record_length, error) ||
            posting.source_byte_offset > record_length ||
            posting.source_byte_length > record_length - posting.source_byte_offset) {
            return fail_index(error, "source-record index posting escapes physical record");
        }
        LogicalNodeRecord node;
        if (!impl_->arena.node_by_ordinal(posting.node_ordinal, &node, error)) {
            return false;
        }
        std::uint64_t node_overlap_end = 0U;
        if (node.source_byte_length == 0U ||
            source_record_index < node.source_record_index ||
            !checked_add(
                posting.node_span_offset,
                posting.source_byte_length,
                &node_overlap_end) ||
            node_overlap_end > node.source_byte_length) {
            return fail_index(error, "source-record index posting escapes logical-node span");
        }
        if (source_record_index == node.source_record_index) {
            if (posting.node_span_offset != 0U ||
                posting.source_byte_offset != node.source_byte_offset) {
                return fail_index(error, "source-record index start-record overlap mismatches node");
            }
        } else if (posting.node_span_offset == 0U || posting.source_byte_offset != 0U) {
            return fail_index(error, "source-record index continuation overlap is invalid");
        }

        page->postings.push_back(LogicalNodeSourceRecordPosting{
            posting.node_ordinal,
            posting.source_record_index,
            posting.source_byte_offset,
            posting.source_byte_length,
            posting.node_span_offset});
        previous_index = cursor;
        previous_node = posting.node_ordinal;
        last_index = cursor;
        cursor = posting.next;
    }

    if (cursor == kNoLogicalNodeSourceRecordPosting && last_index != head.last) {
        return fail_index(error, "source-record index posting chain ends before recorded tail");
    }
    if (start_posting_cursor == kNoLogicalNodeSourceRecordPosting &&
        cursor == kNoLogicalNodeSourceRecordPosting &&
        page->postings.size() != head.count) {
        return fail_index(error, "source-record index head count disagrees with complete chain");
    }
    page->truncated = cursor != kNoLogicalNodeSourceRecordPosting;
    page->next_posting_cursor = cursor;
    return true;
}

} // namespace zevryon::massivedoc
