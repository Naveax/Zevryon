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
        ("zevryon-m4-trigram-find-" + std::string(name));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    REQUIRE(!ignored);
    return root;
}

StoreStats build_store(
    const std::filesystem::path& root,
    std::string_view first_record = "alpha needle omega") {
    StoreConfig config;
    config.segment_bytes = 64U;
    config.records_per_search_block = 2U;
    StoreWriter writer(root, config);
    std::string error;
    const std::array<std::string, 4> records{
        std::string(first_record),
        std::string(44U, 'x') + "BOUNDARY_NEEDLE" + std::string(80U, 'y'),
        "middle record with no marker",
        "tail needle marker",
    };
    for (std::size_t index = 0U; index < records.size(); ++index) {
        const auto bytes = bytes_of(records[index]);
        REQUIRE(writer.append(static_cast<std::uint64_t>(100U + index), bytes, &error));
    }
    StoreStats stats;
    REQUIRE(writer.finalize({}, &stats, &error));
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

void require_needle_hits(const std::vector<SearchHit>& hits) {
    REQUIRE(hits.size() == 2U);
    REQUIRE(hits[0].record_index == 0U);
    REQUIRE(hits[0].logical_id == 100U);
    REQUIRE(hits[0].byte_offset == 6U);
    REQUIRE(hits[1].record_index == 3U);
    REQUIRE(hits[1].logical_id == 103U);
    REQUIRE(hits[1].byte_offset == 5U);
}

void test_accelerated_find_bypasses_legacy_bigram_file() {
    const auto root = test_root("active");
    (void)build_store(root);
    build_derived(root);

    StoreReadConfig read_config;
    read_config.io_window_bytes = 17U;
    StoreReader reader(root, read_config);
    std::string error;
    REQUIRE(reader.open(&error));

    // StoreReader::find() opens search.bgm lazily. Removing it after store open proves
    // that the >=3-byte query completed through the derived trigram candidate path.
    std::error_code fs_error;
    REQUIRE(std::filesystem::remove(root / "search.bgm", fs_error));
    REQUIRE(!fs_error);
    const auto hits = reader.find("needle", 10U, &error);
    REQUIRE(error.empty());
    require_needle_hits(hits);

    const auto limited = reader.find("needle", 1U, &error);
    REQUIRE(error.empty());
    REQUIRE(limited.size() == 1U);
    REQUIRE(limited[0].record_index == 0U);

    std::filesystem::remove_all(root, fs_error);
    REQUIRE(!fs_error);
}

void test_runtime_bloom_corruption_discards_partial_accelerated_hits() {
    const auto root = test_root("runtime-corrupt");
    (void)build_store(root);
    build_derived(root);

    StoreReader reader(root);
    std::string error;
    REQUIRE(reader.open(&error));

    // Corrupt the Bloom frame for record 3. Record 0 is a valid earlier candidate,
    // so the accelerated path may already have accumulated one hit when this checksum
    // failure is observed. The public result must still be a fresh legacy exact search.
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

    const auto hits = reader.find("needle", 10U, &error);
    REQUIRE(error.empty());
    require_needle_hits(hits);

    std::error_code fs_error;
    std::filesystem::remove_all(root, fs_error);
    REQUIRE(!fs_error);
}

void test_stale_other_corpus_index_falls_back_to_legacy() {
    const auto root = test_root("stale-target");
    const auto other = test_root("stale-other");
    (void)build_store(root);
    (void)build_store(other, "different corpus payload");
    build_derived(other);

    std::error_code fs_error;
    std::filesystem::copy_file(
        other / "search.tri",
        root / "search.tri",
        std::filesystem::copy_options::overwrite_existing,
        fs_error);
    REQUIRE(!fs_error);
    std::filesystem::copy_file(
        other / "search.blm",
        root / "search.blm",
        std::filesystem::copy_options::overwrite_existing,
        fs_error);
    REQUIRE(!fs_error);

    StoreReader reader(root);
    std::string error;
    REQUIRE(reader.open(&error));
    const auto hits = reader.find("needle", 10U, &error);
    REQUIRE(error.empty());
    require_needle_hits(hits);

    std::filesystem::remove_all(root, fs_error);
    REQUIRE(!fs_error);
    std::filesystem::remove_all(other, fs_error);
    REQUIRE(!fs_error);
}

void test_short_query_keeps_legacy_semantics() {
    const auto root = test_root("short");
    (void)build_store(root);
    build_derived(root);
    StoreReader reader(root);
    std::string error;
    REQUIRE(reader.open(&error));
    const auto hits = reader.find("ne", 10U, &error);
    REQUIRE(error.empty());
    REQUIRE(hits.size() == 2U);
    REQUIRE(hits[0].record_index == 0U);
    REQUIRE(hits[1].record_index == 3U);
    std::error_code fs_error;
    std::filesystem::remove_all(root, fs_error);
    REQUIRE(!fs_error);
}

} // namespace

int main() {
    test_accelerated_find_bypasses_legacy_bigram_file();
    test_runtime_bloom_corruption_discards_partial_accelerated_hits();
    test_stale_other_corpus_index_falls_back_to_legacy();
    test_short_query_keeps_legacy_semantics();
    std::cout << "Zevryon MassiveDoc trigram find tests passed\n";
    return 0;
}
