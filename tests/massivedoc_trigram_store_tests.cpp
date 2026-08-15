#include "massivedoc_store.hpp"
#include "massivedoc_trigram_index.hpp"
#include "massivedoc_trigram_store.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
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

std::vector<std::byte> bytes_of(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(begin, begin + text.size());
}

std::array<std::uint8_t, kTrigramSourceIdentityBytes> parse_identity(std::string_view hex) {
    REQUIRE(hex.size() == kTrigramSourceIdentityBytes * 2U);
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
    std::array<std::uint8_t, kTrigramSourceIdentityBytes> identity{};
    for (std::size_t index = 0U; index < identity.size(); ++index) {
        const int high = nibble(hex[index * 2U]);
        const int low = nibble(hex[index * 2U + 1U]);
        REQUIRE(high >= 0 && low >= 0);
        identity[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return identity;
}

std::filesystem::path test_root(std::string_view name) {
    const auto root = std::filesystem::temp_directory_path() /
        ("zevryon-m4-trigram-store-" + std::string(name));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    REQUIRE(!ignored);
    return root;
}

StoreStats build_store(const std::filesystem::path& root) {
    StoreConfig config;
    config.segment_bytes = 64U;
    config.records_per_search_block = 2U;
    StoreWriter writer(root, config);
    std::string error;

    const std::array<std::string, 4> records{
        "alpha needle omega",
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
    REQUIRE(stats.corpus.logical_records == records.size());
    REQUIRE(stats.segment_count > 1U);
    return stats;
}

std::set<std::uint64_t> candidates(
    TrigramIndexReader* reader,
    std::string_view query) {
    std::set<std::uint64_t> result;
    std::string error;
    TrigramQueryStats stats;
    REQUIRE(reader->visit_candidate_blocks(
        query,
        [&](std::uint64_t block) {
            result.insert(block);
            return true;
        },
        [] { return false; },
        &stats,
        &error));
    return result;
}

void test_builds_from_canonical_store() {
    const auto root = test_root("build");
    const StoreStats store_stats = build_store(root);

    TrigramIndexConfig config;
    config.io_window_bytes = 31U;
    config.sort_memory_bytes = 64U * 1024U;
    config.merge_fan_in = 2U;
    TrigramIndexStats trigram_stats;
    std::string error;
    REQUIRE(build_store_trigram_index(
        root,
        config,
        [] { return false; },
        &trigram_stats,
        &error));
    REQUIRE(trigram_stats.block_count == store_stats.corpus.logical_records);
    REQUIRE(std::filesystem::is_regular_file(root / "search.tri"));
    REQUIRE(std::filesystem::is_regular_file(root / "search.blm"));

    TrigramIndexReader reader(root, config);
    REQUIRE(reader.open(parse_identity(store_stats.payload_sha256), &error));
    REQUIRE(reader.block_count() == store_stats.corpus.logical_records);

    const auto needle = candidates(&reader, "needle");
    REQUIRE(needle.contains(0U));
    REQUIRE(needle.contains(3U));
    REQUIRE(!needle.contains(2U));

    // Record 0 leaves segment offset 18. Forty-four prefix bytes place the marker
    // two bytes before the next 64-byte segment boundary, so candidate generation
    // must retain trigram state across StoreReader chunk callbacks.
    const auto boundary = candidates(&reader, "BOUNDARY_NEEDLE");
    REQUIRE(boundary.contains(1U));

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    REQUIRE(!ignored);
}

void test_cancelled_build_does_not_publish_final_index() {
    const auto root = test_root("cancel");
    (void)build_store(root);
    TrigramIndexStats stats;
    std::string error;
    int checks = 0;
    REQUIRE(!build_store_trigram_index(
        root,
        {},
        [&checks] { return ++checks >= 1; },
        &stats,
        &error));
    REQUIRE(error == "trigram index build cancelled");
    REQUIRE(!std::filesystem::is_regular_file(root / "search.tri"));
    REQUIRE(!std::filesystem::is_regular_file(root / "search.blm"));

    // Cancellation of an optional derived build must not damage canonical store authority.
    StoreReader reader(root);
    REQUIRE(reader.open(&error));
    REQUIRE(reader.verify(&error));
    const auto hits = reader.find("needle", 10U, &error);
    REQUIRE(error.empty());
    REQUIRE(hits.size() == 2U);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    REQUIRE(!ignored);
}

void test_corrupt_source_is_rejected_before_derived_publication() {
    const auto root = test_root("corrupt-source");
    (void)build_store(root);
    const auto segment = root / "segments" / "segment-00000000.bin";
    {
        std::fstream stream(segment, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(stream);
        char byte = 0;
        stream.read(&byte, 1);
        REQUIRE(stream.gcount() == 1);
        byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0x20U);
        stream.seekp(0, std::ios::beg);
        stream.write(&byte, 1);
        REQUIRE(stream);
    }
    TrigramIndexStats stats;
    std::string error;
    REQUIRE(!build_store_trigram_index(
        root,
        {},
        [] { return false; },
        &stats,
        &error));
    REQUIRE(!std::filesystem::is_regular_file(root / "search.tri"));
    REQUIRE(!std::filesystem::is_regular_file(root / "search.blm"));
    REQUIRE(error.find("integrity") != std::string::npos ||
            error.find("SHA") != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    REQUIRE(!ignored);
}

} // namespace

int main() {
    test_builds_from_canonical_store();
    test_cancelled_build_does_not_publish_final_index();
    test_corrupt_source_is_rejected_before_derived_publication();
    std::cout << "Zevryon MassiveDoc trigram store tests passed\n";
    return 0;
}
