#include "massivedoc_store.hpp"
#include "massivedoc_trigram_store.hpp"

#include <array>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace zevryon::massivedoc;

#define REQUIRE(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "REQUIRE failed: " #condition " at " << __FILE__ << ':' << __LINE__ << "\n"; \
            std::exit(1); \
        } \
    } while (false)

constexpr std::uint64_t kBloomHeaderBytesForTest = 80U;
constexpr std::uint64_t kBloomFrameBytesForTest = 260U;

std::vector<std::byte> bytes_of(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(begin, begin + text.size());
}

std::filesystem::path test_root(std::string_view name) {
    const auto root = std::filesystem::temp_directory_path() /
        ("zevryon-m4-search-cancel-" + std::string(name));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    REQUIRE(!ignored);
    return root;
}

StoreStats build_store(const std::filesystem::path& root) {
    StoreConfig config;
    config.segment_bytes = 64U;
    config.records_per_search_block = 1U;
    StoreWriter writer(root, config);
    std::string error;
    const std::array<std::string, 4> records{
        "alpha needle omega",
        "second block has no marker",
        "third block has no marker",
        "tail needle marker",
    };
    for (std::size_t index = 0U; index < records.size(); ++index) {
        const auto bytes = bytes_of(records[index]);
        REQUIRE(writer.append(static_cast<std::uint64_t>(100U + index), bytes, &error));
    }
    StoreStats stats;
    REQUIRE(writer.finalize({}, &stats, &error));
    REQUIRE(stats.search_block_count == records.size());
    return stats;
}

void build_derived(const std::filesystem::path& root) {
    TrigramIndexConfig config;
    config.io_window_bytes = 31U;
    config.sort_memory_bytes = 64U * 1024U;
    config.merge_fan_in = 2U;
    TrigramIndexStats stats;
    std::string error;
    REQUIRE(build_store_trigram_index(
        root,
        config,
        [] { return false; },
        &stats,
        &error));
    REQUIRE(stats.block_count == 4U);
}

void require_two_needle_hits(const std::vector<SearchHit>& hits) {
    REQUIRE(hits.size() == 2U);
    REQUIRE(hits[0].record_index == 0U);
    REQUIRE(hits[0].logical_id == 100U);
    REQUIRE(hits[0].byte_offset == 6U);
    REQUIRE(hits[1].record_index == 3U);
    REQUIRE(hits[1].logical_id == 103U);
    REQUIRE(hits[1].byte_offset == 5U);
}

void test_trigram_cancellation_never_falls_back_or_leaks_partial_hits() {
    const auto root = test_root("trigram");
    (void)build_store(root);
    build_derived(root);

    StoreReader reader(root);
    std::string error;
    REQUIRE(reader.open(&error));

    // If cancellation were incorrectly treated as a derived-index failure, fallback
    // would try to open this removed legacy file and return the wrong error.
    std::error_code fs_error;
    REQUIRE(std::filesystem::remove(root / "search.bgm", fs_error));
    REQUIRE(!fs_error);

    int checks = 0;
    SearchExecutionStats stats;
    const auto hits = reader.find_bounded(
        "needle",
        10U,
        [&checks] { return ++checks >= 2; },
        &stats,
        &error);
    REQUIRE(hits.empty());
    REQUIRE(error == "search cancelled");
    REQUIRE(stats.used_trigram);
    REQUIRE(!stats.fell_back_from_trigram);
    REQUIRE(stats.cancelled);
    REQUIRE(stats.trigram_candidate_blocks == 1U);
    REQUIRE(stats.exact_records_scanned == 1U);
    REQUIRE(stats.legacy_blocks_checked == 0U);

    std::filesystem::remove_all(root, fs_error);
    REQUIRE(!fs_error);
}

void test_legacy_block_boundary_cancellation_discards_prior_hits() {
    const auto root = test_root("legacy");
    (void)build_store(root);

    StoreReader reader(root);
    std::string error;
    REQUIRE(reader.open(&error));
    int checks = 0;
    SearchExecutionStats stats;
    const auto hits = reader.find_bounded(
        "needle",
        10U,
        [&checks] { return ++checks >= 2; },
        &stats,
        &error);
    REQUIRE(hits.empty());
    REQUIRE(error == "search cancelled");
    REQUIRE(!stats.used_trigram);
    REQUIRE(stats.fell_back_from_trigram);
    REQUIRE(stats.cancelled);
    REQUIRE(stats.legacy_blocks_checked == 1U);
    REQUIRE(stats.exact_records_scanned == 1U);

    std::error_code fs_error;
    std::filesystem::remove_all(root, fs_error);
    REQUIRE(!fs_error);
}

void test_derived_corruption_is_fallback_not_cancellation() {
    const auto root = test_root("fallback");
    (void)build_store(root);
    build_derived(root);

    StoreReader reader(root);
    std::string error;
    REQUIRE(reader.open(&error));

    const std::uint64_t corrupt_offset =
        kBloomHeaderBytesForTest + 3U * kBloomFrameBytesForTest;
    {
        std::fstream stream(
            root / "search.blm",
            std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(stream);
        stream.seekg(static_cast<std::streamoff>(corrupt_offset), std::ios::beg);
        char byte = 0;
        stream.read(&byte, 1);
        REQUIRE(stream.gcount() == 1);
        byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0x01U);
        stream.seekp(static_cast<std::streamoff>(corrupt_offset), std::ios::beg);
        stream.write(&byte, 1);
        REQUIRE(stream);
    }

    SearchExecutionStats stats;
    const auto hits = reader.find_bounded(
        "needle",
        10U,
        [] { return false; },
        &stats,
        &error);
    REQUIRE(error.empty());
    require_two_needle_hits(hits);
    REQUIRE(stats.used_trigram);
    REQUIRE(stats.fell_back_from_trigram);
    REQUIRE(!stats.cancelled);
    REQUIRE(stats.trigram_candidate_blocks >= 1U);
    REQUIRE(stats.legacy_blocks_checked >= 1U);

    std::error_code fs_error;
    std::filesystem::remove_all(root, fs_error);
    REQUIRE(!fs_error);
}

void test_legacy_find_wrapper_remains_equivalent() {
    const auto root = test_root("wrapper");
    (void)build_store(root);
    build_derived(root);
    StoreReader reader(root);
    std::string error;
    REQUIRE(reader.open(&error));

    const auto legacy_api = reader.find("needle", 10U, &error);
    REQUIRE(error.empty());
    SearchExecutionStats stats;
    const auto bounded = reader.find_bounded(
        "needle",
        10U,
        {},
        &stats,
        &error);
    REQUIRE(error.empty());
    require_two_needle_hits(legacy_api);
    require_two_needle_hits(bounded);
    REQUIRE(stats.used_trigram);
    REQUIRE(!stats.cancelled);

    std::error_code fs_error;
    std::filesystem::remove_all(root, fs_error);
    REQUIRE(!fs_error);
}

} // namespace

int main() {
    test_trigram_cancellation_never_falls_back_or_leaks_partial_hits();
    test_legacy_block_boundary_cancellation_discards_prior_hits();
    test_derived_corruption_is_fallback_not_cancellation();
    test_legacy_find_wrapper_remains_equivalent();
    std::cout << "Zevryon MassiveDoc search cancellation tests passed\n";
    return 0;
}
