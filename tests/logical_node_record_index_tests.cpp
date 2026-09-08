#include "logical_node_arena.hpp"
#include "logical_node_arena_v2_store_bound.hpp"
#include "logical_node_record_index.hpp"
#include "logical_node_source.hpp"
#include "massivedoc_store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

using zevryon::massivedoc::CorpusMetadata;
using zevryon::massivedoc::LogicalNodeArenaBuildConfig;
using zevryon::massivedoc::LogicalNodeArenaV2StoreBoundWriter;
using zevryon::massivedoc::LogicalNodeInput;
using zevryon::massivedoc::LogicalNodeRecordIndexReader;
using zevryon::massivedoc::LogicalNodeRecordIndexWindow;
using zevryon::massivedoc::LogicalNodeSourceStoreBinding;
using zevryon::massivedoc::StoreWriter;
using zevryon::massivedoc::build_logical_node_record_index;
using zevryon::massivedoc::inspect_logical_node_source_store_binding;
using zevryon::massivedoc::kNoLogicalNodeOrdinal;
using zevryon::massivedoc::kNoLogicalNodeRecordPosting;

constexpr std::size_t kPostingBytes = 48U;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: logical node record index: " << message << '\n';
        return false;
    }
    return true;
}

std::filesystem::path unique_root(std::string_view name) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (std::string("zevryon-") + std::string(name) + "-" +
         std::to_string(tick));
}

struct RootCleanup {
    explicit RootCleanup(std::filesystem::path value) : root(std::move(value)) {}
    ~RootCleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    RootCleanup(const RootCleanup&) = delete;
    RootCleanup& operator=(const RootCleanup&) = delete;
    std::filesystem::path root;
};

std::span<const std::byte> bytes(std::string_view text) {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()),
        text.size());
}

bool build_store(const std::filesystem::path& root, std::string* error) {
    constexpr std::array<std::string_view, 3> records{{"ab", "cdef", "gh"}};
    StoreWriter writer(root);
    for (std::size_t index = 0U; index < records.size(); ++index) {
        if (!writer.append(
                9701U + static_cast<std::uint64_t>(index),
                bytes(records[index]),
                error)) {
            return false;
        }
    }
    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = 8U;
    metadata.logical_records = 3U;
    metadata.logical_nodes = 4U;
    metadata.largest_record_bytes = 4U;
    return writer.finalize(metadata, nullptr, error);
}

bool build_arena(
    const std::filesystem::path& root,
    std::string_view candidate_tree,
    std::string* error) {
    LogicalNodeSourceStoreBinding binding;
    if (!inspect_logical_node_source_store_binding(root, &binding, error)) {
        return false;
    }
    LogicalNodeArenaBuildConfig config;
    config.candidate_commit =
        "0123456789abcdef0123456789abcdef01234567";
    config.candidate_tree = std::string(candidate_tree);
    config.source_sha256 = binding.payload_sha256;
    config.semantic_bucket_count = 64U;
    config.semantic_hash_bits = 64U;

    LogicalNodeArenaV2StoreBoundWriter writer(root, config);
    return writer.begin(error) &&
        writer.append_node(
            LogicalNodeInput{
                1U, 0U, 0U, 0U, kNoLogicalNodeOrdinal,
                "#document", "", "", 0U},
            {},
            error) &&
        writer.append_node(
            LogicalNodeInput{
                2U, 0U, 0U, 1U, 0U,
                "div", "", "", 0U},
            {},
            error) &&
        writer.append_node(
            LogicalNodeInput{
                3U, 0U, 1U, 6U, 1U,
                "#text", "", "", 0U},
            {},
            error) &&
        writer.append_node(
            LogicalNodeInput{
                4U, 1U, 1U, 2U, 1U,
                "span", "note", "display:inline", 0U},
            {},
            error) &&
        writer.finish(error);
}

std::uint32_t crc32(std::span<const std::uint8_t> input) {
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t byte : input) {
        crc ^= static_cast<std::uint32_t>(byte);
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const std::uint32_t mask =
                static_cast<std::uint32_t>(0U - (crc & 1U));
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

void put_u64(
    std::array<std::uint8_t, kPostingBytes>* bytes,
    std::size_t offset,
    std::uint64_t value) {
    for (std::size_t index = 0U; index < 8U; ++index) {
        (*bytes)[offset + index] = static_cast<std::uint8_t>(
            (value >> (index * 8U)) & 0xffU);
    }
}

void put_u32(
    std::array<std::uint8_t, kPostingBytes>* bytes,
    std::size_t offset,
    std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index) {
        (*bytes)[offset + index] = static_cast<std::uint8_t>(
            (value >> (index * 8U)) & 0xffU);
    }
}

bool rewrite_posting(
    const std::filesystem::path& path,
    std::uint64_t posting_ordinal,
    std::uint64_t node_ordinal,
    std::uint64_t next_posting,
    std::uint64_t source_record_index) {
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) {
        return false;
    }
    const std::uint64_t offset =
        posting_ordinal * static_cast<std::uint64_t>(kPostingBytes);
    if (offset >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::streamoff>::max())) {
        return false;
    }
    std::array<std::uint8_t, kPostingBytes> raw{};
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    stream.read(
        reinterpret_cast<char*>(raw.data()),
        static_cast<std::streamsize>(raw.size()));
    if (!stream) {
        return false;
    }
    put_u64(&raw, 0U, node_ordinal);
    put_u64(&raw, 8U, next_posting);
    put_u64(&raw, 16U, source_record_index);
    put_u32(&raw, 40U, 0U);
    put_u32(
        &raw,
        44U,
        crc32(std::span<const std::uint8_t>(raw.data(), 44U)));
    stream.clear();
    stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    stream.write(
        reinterpret_cast<const char*>(raw.data()),
        static_cast<std::streamsize>(raw.size()));
    stream.flush();
    return static_cast<bool>(stream);
}

bool test_cross_record_round_trip_and_continuation() {
    const std::filesystem::path root = unique_root("record-node-index-roundtrip");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_store(root, &error), error) ||
        !require(build_arena(
            root,
            "89abcdef0123456789abcdef0123456789abcdef",
            &error), error) ||
        !require(build_logical_node_record_index(root, &error), error)) {
        return false;
    }

    LogicalNodeRecordIndexReader reader(root);
    if (!require(reader.open(&error), error) ||
        !require(reader.manifest().source_record_count == 3U, "record count") ||
        !require(reader.manifest().node_count == 4U, "node count") ||
        !require(reader.manifest().posting_count == 5U, "posting count")) {
        return false;
    }

    LogicalNodeRecordIndexWindow first;
    if (!require(reader.read_record(
            0U, kNoLogicalNodeRecordPosting, 1U, &first, &error), error) ||
        !require(first.postings.size() == 1U, "first page has one posting") ||
        !require(first.postings[0].node_ordinal == 1U, "first record keeps node order") ||
        !require(first.postings[0].record_byte_offset == 0U &&
                     first.postings[0].record_byte_length == 1U,
                 "first element overlap") ||
        !require(first.truncated, "first page truncates") ||
        !require(first.next_posting_ordinal != kNoLogicalNodeRecordPosting,
                 "first page returns continuation")) {
        return false;
    }

    LogicalNodeRecordIndexWindow second;
    if (!require(reader.read_record(
            0U, first.next_posting_ordinal, 8U, &second, &error), error) ||
        !require(second.postings.size() == 1U, "continuation returns remaining posting") ||
        !require(second.postings[0].node_ordinal == 2U, "cross-record node follows") ||
        !require(second.postings[0].record_byte_offset == 1U &&
                     second.postings[0].record_byte_length == 1U,
                 "cross-record first overlap") ||
        !require(!second.truncated, "continuation reaches chain end")) {
        return false;
    }

    LogicalNodeRecordIndexWindow middle;
    LogicalNodeRecordIndexWindow tail;
    return require(reader.read_record(
               1U, kNoLogicalNodeRecordPosting, 8U, &middle, &error), error) &&
        require(middle.postings.size() == 2U, "middle record has two nodes") &&
        require(middle.postings[0].node_ordinal == 2U &&
                    middle.postings[0].record_byte_offset == 0U &&
                    middle.postings[0].record_byte_length == 4U,
                "cross-record middle overlap") &&
        require(middle.postings[1].node_ordinal == 3U &&
                    middle.postings[1].record_byte_offset == 1U &&
                    middle.postings[1].record_byte_length == 2U,
                "record-local second node overlap") &&
        require(reader.read_record(
               2U, kNoLogicalNodeRecordPosting, 8U, &tail, &error), error) &&
        require(tail.postings.size() == 1U &&
                    tail.postings[0].node_ordinal == 2U &&
                    tail.postings[0].record_byte_offset == 0U &&
                    tail.postings[0].record_byte_length == 1U,
                "cross-record tail overlap");
}

bool test_foreign_record_continuation_rejected() {
    const std::filesystem::path root = unique_root("record-node-index-foreign-continuation");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_store(root, &error), error) ||
        !require(build_arena(
            root,
            "89abcdef0123456789abcdef0123456789abcdef",
            &error), error) ||
        !require(build_logical_node_record_index(root, &error), error)) {
        return false;
    }

    LogicalNodeRecordIndexReader reader(root);
    LogicalNodeRecordIndexWindow record_one;
    if (!require(reader.open(&error), error) ||
        !require(reader.read_record(
            1U, kNoLogicalNodeRecordPosting, 1U, &record_one, &error), error) ||
        !require(record_one.postings.size() == 1U,
                 "record one exposes one posting for forged continuation")) {
        return false;
    }

    LogicalNodeRecordIndexWindow forged;
    return require(!reader.read_record(
               0U,
               record_one.postings[0].posting_ordinal,
               8U,
               &forged,
               &error),
            "foreign-record continuation is rejected") &&
        require(error.find("different source record") != std::string::npos,
                "foreign-record continuation failure is explicit");
}

bool test_forged_overlap_and_cycle_fail_closed() {
    const std::filesystem::path root = unique_root("record-node-index-corruption");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_store(root, &error), error) ||
        !require(build_arena(
            root,
            "89abcdef0123456789abcdef0123456789abcdef",
            &error), error) ||
        !require(build_logical_node_record_index(root, &error), error)) {
        return false;
    }

    std::uint64_t record_two_posting = kNoLogicalNodeRecordPosting;
    std::uint64_t record_zero_first = kNoLogicalNodeRecordPosting;
    {
        LogicalNodeRecordIndexReader reader(root);
        LogicalNodeRecordIndexWindow record_two;
        LogicalNodeRecordIndexWindow record_zero;
        if (!require(reader.open(&error), error) ||
            !require(reader.read_record(
                2U, kNoLogicalNodeRecordPosting, 8U, &record_two, &error), error) ||
            !require(reader.read_record(
                0U, kNoLogicalNodeRecordPosting, 1U, &record_zero, &error), error)) {
            return false;
        }
        record_two_posting = record_two.postings[0].posting_ordinal;
        record_zero_first = record_zero.postings[0].posting_ordinal;
    }

    const std::filesystem::path postings =
        root / "node-record-index-v1" / "postings.bin";
    if (!require(rewrite_posting(
            postings,
            record_two_posting,
            3U,
            kNoLogicalNodeRecordPosting,
            2U),
            "rewrite posting with valid CRC")) {
        return false;
    }
    {
        LogicalNodeRecordIndexReader reader(root);
        LogicalNodeRecordIndexWindow result;
        if (!require(reader.open(&error), error) ||
            !require(!reader.read_record(
                2U, kNoLogicalNodeRecordPosting, 8U, &result, &error),
                "forged non-overlap posting rejected") ||
            !require(error.find("does not overlap") != std::string::npos,
                "forged overlap failure is explicit")) {
            return false;
        }
    }

    std::error_code remove_error;
    std::filesystem::remove_all(root / "node-record-index-v1", remove_error);
    if (!require(!remove_error, "remove corrupted index") ||
        !require(build_logical_node_record_index(root, &error), error)) {
        return false;
    }
    {
        LogicalNodeRecordIndexReader reader(root);
        LogicalNodeRecordIndexWindow record_zero;
        if (!require(reader.open(&error), error) ||
            !require(reader.read_record(
                0U, kNoLogicalNodeRecordPosting, 1U, &record_zero, &error), error)) {
            return false;
        }
        record_zero_first = record_zero.postings[0].posting_ordinal;
    }
    if (!require(rewrite_posting(
            postings,
            record_zero_first,
            1U,
            record_zero_first,
            0U),
            "rewrite posting into self-cycle with valid CRC")) {
        return false;
    }
    {
        LogicalNodeRecordIndexReader reader(root);
        LogicalNodeRecordIndexWindow result;
        return require(reader.open(&error), error) &&
            require(!reader.read_record(
                0U, kNoLogicalNodeRecordPosting, 8U, &result, &error),
                "self-cycle/back-link rejected") &&
            require(error.find("strictly forward") != std::string::npos,
                "cycle failure is explicit");
    }
}

bool test_arena_identity_change_rejected() {
    const std::filesystem::path root = unique_root("record-node-index-arena-id");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_store(root, &error), error) ||
        !require(build_arena(
            root,
            "89abcdef0123456789abcdef0123456789abcdef",
            &error), error) ||
        !require(build_logical_node_record_index(root, &error), error)) {
        return false;
    }

    std::error_code remove_error;
    std::filesystem::remove_all(root / "node-arena-v2", remove_error);
    if (!require(!remove_error, "remove original arena") ||
        !require(build_arena(
            root,
            "fedcba9876543210fedcba9876543210fedcba98",
            &error), error)) {
        return false;
    }

    LogicalNodeRecordIndexReader reader(root);
    return require(!reader.open(&error), "arena identity change rejected") &&
        require(error.find("arena identity mismatch") != std::string::npos,
                "arena identity mismatch is explicit");
}

bool test_truncated_posting_table_rejected_on_open() {
    const std::filesystem::path root = unique_root("record-node-index-truncate");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_store(root, &error), error) ||
        !require(build_arena(
            root,
            "89abcdef0123456789abcdef0123456789abcdef",
            &error), error) ||
        !require(build_logical_node_record_index(root, &error), error)) {
        return false;
    }

    const std::filesystem::path postings =
        root / "node-record-index-v1" / "postings.bin";
    const std::uintmax_t size = std::filesystem::file_size(postings);
    if (!require(size > 0U, "posting table is non-empty")) {
        return false;
    }
    std::filesystem::resize_file(postings, size - 1U);

    LogicalNodeRecordIndexReader reader(root);
    return require(!reader.open(&error), "truncated posting table rejected");
}

} // namespace

int main() {
    if (!test_cross_record_round_trip_and_continuation() ||
        !test_foreign_record_continuation_rejected() ||
        !test_forged_overlap_and_cycle_fail_closed() ||
        !test_arena_identity_change_rejected() ||
        !test_truncated_posting_table_rejected_on_open()) {
        return 1;
    }
    return 0;
}
