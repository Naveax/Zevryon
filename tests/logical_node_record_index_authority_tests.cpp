#include "logical_node_arena.hpp"
#include "logical_node_arena_v2_store_bound.hpp"
#include "logical_node_record_index.hpp"
#include "logical_node_source.hpp"
#include "massivedoc_store.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

using zevryon::massivedoc::CorpusMetadata;
using zevryon::massivedoc::LogicalNodeArenaBuildConfig;
using zevryon::massivedoc::LogicalNodeArenaV2StoreBoundWriter;
using zevryon::massivedoc::LogicalNodeInput;
using zevryon::massivedoc::LogicalNodeRecordIndexAuthoritativeReader;
using zevryon::massivedoc::LogicalNodeRecordIndexReader;
using zevryon::massivedoc::LogicalNodeRecordIndexWindow;
using zevryon::massivedoc::LogicalNodeSourceStoreBinding;
using zevryon::massivedoc::StoreWriter;
using zevryon::massivedoc::build_logical_node_record_index;
using zevryon::massivedoc::inspect_logical_node_source_store_binding;
using zevryon::massivedoc::kNoLogicalNodeOrdinal;
using zevryon::massivedoc::kNoLogicalNodeRecordPosting;

constexpr std::size_t kHeadBytes = 32U;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: logical node record-index authority: "
                  << message << '\n';
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
        reinterpret_cast<const std::byte*>(text.data()), text.size());
}

bool build_store(const std::filesystem::path& root, std::string* error) {
    constexpr std::array<std::string_view, 3> records{{"ab", "cdef", "gh"}};
    StoreWriter writer(root);
    for (std::size_t index = 0U; index < records.size(); ++index) {
        if (!writer.append(
                9801U + static_cast<std::uint64_t>(index),
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

bool build_arena(const std::filesystem::path& root, std::string* error) {
    LogicalNodeSourceStoreBinding binding;
    if (!inspect_logical_node_source_store_binding(root, &binding, error)) {
        return false;
    }
    LogicalNodeArenaBuildConfig config;
    config.candidate_commit =
        "0123456789abcdef0123456789abcdef01234567";
    config.candidate_tree =
        "89abcdef0123456789abcdef0123456789abcdef";
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

template <typename T>
void put_le(
    std::array<std::uint8_t, kHeadBytes>* raw,
    std::size_t offset,
    T value) {
    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        (*raw)[offset + index] = static_cast<std::uint8_t>(
            (value >> (index * 8U)) & static_cast<T>(0xffU));
    }
}

bool rewrite_head(
    const std::filesystem::path& root,
    std::uint64_t source_record_index,
    std::uint64_t first_posting,
    std::uint64_t last_posting,
    std::uint64_t posting_count) {
    const std::filesystem::path path =
        root / "node-record-index-v1" / "heads.bin";
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) {
        return false;
    }
    const std::uint64_t offset =
        source_record_index * static_cast<std::uint64_t>(kHeadBytes);
    std::array<std::uint8_t, kHeadBytes> raw{};
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    stream.read(
        reinterpret_cast<char*>(raw.data()),
        static_cast<std::streamsize>(raw.size()));
    if (!stream) {
        return false;
    }

    put_le(&raw, 0U, first_posting);
    put_le(&raw, 8U, last_posting);
    put_le(&raw, 16U, posting_count);
    put_le(&raw, 24U, std::uint32_t{0U});
    put_le(
        &raw,
        28U,
        crc32(std::span<const std::uint8_t>(raw.data(), 28U)));

    stream.clear();
    stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    stream.write(
        reinterpret_cast<const char*>(raw.data()),
        static_cast<std::streamsize>(raw.size()));
    stream.flush();
    return static_cast<bool>(stream);
}

bool build_fixture(const std::filesystem::path& root, std::string* error) {
    return build_store(root, error) &&
        build_arena(root, error) &&
        build_logical_node_record_index(root, error);
}

bool test_normal_authoritative_pagination() {
    const std::filesystem::path root = unique_root("record-index-authority-ok");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_fixture(root, &error), error)) {
        return false;
    }

    LogicalNodeRecordIndexAuthoritativeReader reader(root);
    LogicalNodeRecordIndexWindow first;
    LogicalNodeRecordIndexWindow second;
    return require(reader.open(&error), error) &&
        require(reader.read_record(
            0U, kNoLogicalNodeRecordPosting, 1U, &first, &error), error) &&
        require(first.postings.size() == 1U && first.truncated,
                "authoritative first page truncates at complete posting") &&
        require(reader.read_record(
            0U, first.next_posting_ordinal, 8U, &second, &error), error) &&
        require(second.postings.size() == 1U && !second.truncated,
                "authoritative continuation reaches frozen head tail");
}

bool test_crc_valid_forged_last_posting_rejected() {
    const std::filesystem::path root = unique_root("record-index-forged-tail");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_fixture(root, &error), error) ||
        !require(rewrite_head(root, 0U, 0U, 2U, 2U),
                 "forge CRC-valid head last posting")) {
        return false;
    }

    // The low-level storage reader intentionally does not define head-tail
    // authority. This demonstrates that the production wrapper adds a real
    // integrity condition rather than merely duplicating the same call.
    {
        LogicalNodeRecordIndexReader storage(root);
        LogicalNodeRecordIndexWindow result;
        if (!require(storage.open(&error), error) ||
            !require(storage.read_record(
                0U, kNoLogicalNodeRecordPosting, 8U, &result, &error),
                "low-level chain remains structurally readable")) {
            return false;
        }
    }

    LogicalNodeRecordIndexAuthoritativeReader authoritative(root);
    LogicalNodeRecordIndexWindow result;
    return require(authoritative.open(&error), error) &&
        require(!authoritative.read_record(
            0U, kNoLogicalNodeRecordPosting, 8U, &result, &error),
            "CRC-valid forged last posting rejected") &&
        require(error.find("chain tail disagrees") != std::string::npos,
                "forged tail failure is explicit");
}

bool test_non_empty_head_with_missing_endpoint_rejected() {
    const std::filesystem::path root = unique_root("record-index-missing-head-endpoint");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_fixture(root, &error), error) ||
        !require(rewrite_head(
            root,
            0U,
            kNoLogicalNodeRecordPosting,
            1U,
            2U),
            "forge CRC-valid non-empty head with missing first posting")) {
        return false;
    }

    LogicalNodeRecordIndexAuthoritativeReader reader(root);
    LogicalNodeRecordIndexWindow result;
    return require(reader.open(&error), error) &&
        require(!reader.read_record(
            0U, kNoLogicalNodeRecordPosting, 8U, &result, &error),
            "non-empty head with missing endpoint rejected") &&
        require(error.find("misses first/last posting") != std::string::npos,
                "missing head endpoint failure is explicit");
}

} // namespace

int main() {
    if (!test_normal_authoritative_pagination() ||
        !test_crc_valid_forged_last_posting_rejected() ||
        !test_non_empty_head_with_missing_endpoint_rejected()) {
        return 1;
    }
    return 0;
}
