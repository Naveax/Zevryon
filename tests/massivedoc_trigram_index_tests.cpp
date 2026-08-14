#include "massivedoc_trigram_index.hpp"

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


std::span<const std::byte> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

std::array<std::uint8_t, kTrigramSourceIdentityBytes> fixture_identity() {
    std::array<std::uint8_t, kTrigramSourceIdentityBytes> identity{};
    for (std::size_t index = 0U; index < identity.size(); ++index) {
        identity[index] = static_cast<std::uint8_t>(index * 7U + 3U);
    }
    return identity;
}

template <typename T>
T read_le(std::istream& stream) {
    std::array<std::byte, sizeof(T)> bytes{};
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(stream.gcount() == static_cast<std::streamsize>(bytes.size()));
    T value = 0U;
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        value |= static_cast<T>(std::to_integer<unsigned char>(bytes[index])) << (index * 8U);
    }
    return value;
}

std::uint32_t trigram_value(char a, char b, char c) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8U) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(c));
}

std::uint64_t posting_file_offset_for(
    const std::filesystem::path& root,
    std::uint32_t wanted) {
    std::ifstream stream(root / "search.tri", std::ios::binary);
    REQUIRE(stream);
    stream.seekg(32, std::ios::beg);
    const std::uint64_t distinct = read_le<std::uint64_t>(stream);
    stream.seekg(48, std::ios::beg);
    const std::uint64_t directory_offset = read_le<std::uint64_t>(stream);
    const std::uint64_t postings_offset = read_le<std::uint64_t>(stream);
    for (std::uint64_t index = 0U; index < distinct; ++index) {
        stream.seekg(static_cast<std::streamoff>(directory_offset + index * 40U), std::ios::beg);
        const std::uint32_t trigram = read_le<std::uint32_t>(stream);
        (void)read_le<std::uint32_t>(stream); // posting CRC
        const std::uint64_t relative = read_le<std::uint64_t>(stream);
        if (trigram == wanted) {
            return postings_offset + relative;
        }
    }
    REQUIRE(false);
    return 0U;
}

struct Fixture {
    std::filesystem::path root;
    std::vector<std::vector<std::string>> blocks;
};

Fixture build_fixture() {
    Fixture fixture;
    fixture.root = std::filesystem::temp_directory_path() / "zevryon-m4-trigram-index-tests";
    std::error_code ignored;
    std::filesystem::remove_all(fixture.root, ignored);
    std::filesystem::create_directories(fixture.root);
    fixture.blocks = {
        {"alpha needle omega", "record-boundary-left"},
        {"beta middle gamma", "record-boundary-right"},
        {"the quick brown fox", "needle again"},
        {"1234567890", "xxabcxx"},
        {"suffix-only-nee", "dle-prefix-must-not-cross-records"},
        {"nothing useful here"},
    };

    TrigramIndexConfig config;
    config.sort_memory_bytes = 64U * 1024U;
    config.merge_fan_in = 2U;
    TrigramIndexWriter writer(fixture.root, config);
    std::string error;
    for (std::size_t block = 0U; block < fixture.blocks.size(); ++block) {
        for (const auto& record : fixture.blocks[block]) {
            REQUIRE(writer.begin_record(static_cast<std::uint64_t>(block), &error));
            // Force feed boundaries through trigrams.
            const auto bytes = bytes_of(record);
            std::size_t offset = 0U;
            const std::array<std::size_t, 4> pattern{1U, 2U, 3U, 5U};
            std::size_t turn = 0U;
            while (offset < bytes.size()) {
                const std::size_t amount = std::min(pattern[turn % pattern.size()], bytes.size() - offset);
                REQUIRE(writer.feed(bytes.subspan(offset, amount), &error));
                offset += amount;
                ++turn;
            }
            REQUIRE(writer.end_record(&error));
        }
    }
    TrigramIndexStats stats;
    const auto source_identity = fixture_identity();
    REQUIRE(writer.finish(
        static_cast<std::uint64_t>(fixture.blocks.size()),
        source_identity,
        &stats,
        [] { return false; },
        &error));
    REQUIRE(stats.block_count == fixture.blocks.size());
    REQUIRE(stats.distinct_trigrams != 0U);
    REQUIRE(stats.posting_pairs != 0U);
    REQUIRE(stats.bloom_bytes == 80U + fixture.blocks.size() * (kTrigramBloomBytes + sizeof(std::uint32_t)));
    REQUIRE(stats.peak_sort_entries != 0U);
    return fixture;
}

std::set<std::uint64_t> exact_blocks(const Fixture& fixture, std::string_view query) {
    std::set<std::uint64_t> result;
    for (std::size_t block = 0U; block < fixture.blocks.size(); ++block) {
        for (const auto& record : fixture.blocks[block]) {
            if (record.find(query) != std::string::npos) {
                result.insert(static_cast<std::uint64_t>(block));
                break;
            }
        }
    }
    return result;
}

std::set<std::uint64_t> candidate_blocks(
    TrigramIndexReader* reader,
    std::string_view query,
    TrigramQueryStats* stats = nullptr) {
    std::set<std::uint64_t> result;
    std::string error;
    TrigramQueryStats local;
    const bool ok = reader->visit_candidate_blocks(
        query,
        [&](std::uint64_t block) {
            result.insert(block);
            return true;
        },
        [] { return false; },
        stats != nullptr ? stats : &local,
        &error);
    if (!ok) {
        std::cerr << "candidate query failed for [" << query << "]: " << error << "\n";
    }
    REQUIRE(ok);
    return result;
}

void test_equivalence_no_false_negatives() {
    const Fixture fixture = build_fixture();
    TrigramIndexConfig config;
    config.io_window_bytes = 17U;
    TrigramIndexReader reader(fixture.root, config);
    std::string error;
    REQUIRE(reader.open(fixture_identity(), &error));
    REQUIRE(reader.block_count() == fixture.blocks.size());

    const std::array<std::string_view, 10> queries{
        "needle", "alpha", "quick brown", "abc", "7890", "middle",
        "record-boundary", "nothing", "does-not-exist", "dle-prefix"};
    for (const auto query : queries) {
        const auto exact = exact_blocks(fixture, query);
        const auto candidates = candidate_blocks(&reader, query);
        for (const auto block : exact) {
            REQUIRE(candidates.contains(block));
        }
    }

    // Trigrams must never bridge records. This query exists only if the end of one record
    // is concatenated with the beginning of the next record in block 4.
    const auto cross_record = candidate_blocks(&reader, "needle-prefix");
    REQUIRE(!cross_record.contains(4U));

    TrigramQueryStats stats;
    const auto needle_candidates = candidate_blocks(&reader, "needle", &stats);
    REQUIRE(needle_candidates.contains(0U));
    REQUIRE(needle_candidates.contains(2U));
    REQUIRE(stats.posting_streams_opened >= 1U);
    REQUIRE(stats.posting_streams_opened <= kTrigramIntersectionStreams);
}

void test_short_query_visits_all_blocks() {
    const Fixture fixture = build_fixture();
    TrigramIndexReader reader(fixture.root);
    std::string error;
    REQUIRE(reader.open(fixture_identity(), &error));
    const auto candidates = candidate_blocks(&reader, "ab");
    REQUIRE(candidates.size() == fixture.blocks.size());
}

void test_cancellation_is_observed() {
    const Fixture fixture = build_fixture();
    TrigramIndexReader reader(fixture.root);
    std::string error;
    REQUIRE(reader.open(fixture_identity(), &error));
    int checks = 0;
    TrigramQueryStats stats;
    const bool ok = reader.visit_candidate_blocks(
        "needle",
        [](std::uint64_t) { return true; },
        [&checks] { return ++checks >= 1; },
        &stats,
        &error);
    REQUIRE(!ok);
    REQUIRE(stats.cancelled);
    REQUIRE(error == "trigram query cancelled");
}


void test_external_sort_merge_path() {
    const auto root = std::filesystem::temp_directory_path() / "zevryon-m4-trigram-merge-tests";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);

    TrigramIndexConfig config;
    config.sort_memory_bytes = 64U * 1024U;
    config.merge_fan_in = 2U;
    TrigramIndexWriter writer(root, config);
    std::string error;
    std::string payload;
    payload.resize(30000U);
    for (std::size_t index = 0U; index < payload.size(); ++index) {
        payload[index] = static_cast<char>(static_cast<unsigned int>('a') + static_cast<unsigned int>((index * 17U + index / 13U) % 26U));
    }
    payload.replace(21000U, 12U, "merge-needle");
    REQUIRE(writer.begin_record(0U, &error));
    const auto bytes = bytes_of(payload);
    for (std::size_t offset = 0U; offset < bytes.size();) {
        const std::size_t amount = std::min<std::size_t>(37U, bytes.size() - offset);
        REQUIRE(writer.feed(bytes.subspan(offset, amount), &error));
        offset += amount;
    }
    REQUIRE(writer.end_record(&error));
    TrigramIndexStats stats;
    const auto source_identity = fixture_identity();
    REQUIRE(writer.finish(1U, source_identity, &stats, [] { return false; }, &error));
    REQUIRE(stats.spool_bytes > 64U * 1024U);
    REQUIRE(stats.merge_passes >= 1U);
    REQUIRE(stats.peak_sort_entries <= (64U * 1024U) / 16U);

    TrigramIndexReader reader(root, config);
    REQUIRE(reader.open(fixture_identity(), &error));
    const auto candidates = candidate_blocks(&reader, "merge-needle");
    REQUIRE(candidates == std::set<std::uint64_t>{0U});
}

void test_corrupt_selected_posting_fails_before_candidates_escape() {
    const Fixture fixture = build_fixture();
    const std::uint64_t offset = posting_file_offset_for(
        fixture.root, trigram_value('n', 'e', 'e'));
    {
        std::fstream stream(
            fixture.root / "search.tri",
            std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(stream);
        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        char byte = 0;
        stream.read(&byte, 1);
        REQUIRE(stream.gcount() == 1);
        byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0x01U);
        stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
        stream.write(&byte, 1);
        REQUIRE(stream);
    }
    TrigramIndexReader reader(fixture.root);
    std::string error;
    REQUIRE(reader.open(fixture_identity(), &error));
    std::size_t emitted = 0U;
    TrigramQueryStats stats;
    const bool ok = reader.visit_candidate_blocks(
        "needle",
        [&emitted](std::uint64_t) {
            ++emitted;
            return true;
        },
        [] { return false; },
        &stats,
        &error);
    REQUIRE(!ok);
    REQUIRE(emitted == 0U);
    REQUIRE(error == "trigram posting checksum mismatch");
}

void test_corrupt_bloom_fails_before_candidate_escape() {
    const Fixture fixture = build_fixture();
    {
        std::fstream stream(
            fixture.root / "search.blm",
            std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(stream);
        stream.seekg(80, std::ios::beg);
        char byte = 0;
        stream.read(&byte, 1);
        REQUIRE(stream.gcount() == 1);
        byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0x01U);
        stream.seekp(80, std::ios::beg);
        stream.write(&byte, 1);
        REQUIRE(stream);
    }
    TrigramIndexReader reader(fixture.root);
    std::string error;
    REQUIRE(reader.open(fixture_identity(), &error));
    std::size_t emitted = 0U;
    TrigramQueryStats stats;
    const bool ok = reader.visit_candidate_blocks(
        "needle",
        [&emitted](std::uint64_t) {
            ++emitted;
            return true;
        },
        [] { return false; },
        &stats,
        &error);
    REQUIRE(!ok);
    REQUIRE(emitted == 0U);
    REQUIRE(error == "trigram Bloom checksum mismatch");
}

void test_source_identity_mismatch_fails_closed() {
    const Fixture fixture = build_fixture();
    auto wrong = fixture_identity();
    wrong[0] ^= 0xffU;
    TrigramIndexReader reader(fixture.root);
    std::string error;
    REQUIRE(!reader.open(wrong, &error));
    REQUIRE(error == "trigram source identity mismatch");
}

void test_corrupt_header_fails_closed() {
    const Fixture fixture = build_fixture();
    {
        std::fstream stream(fixture.root / "search.tri", std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(stream);
        const char bad = 'X';
        stream.write(&bad, 1);
        REQUIRE(stream);
    }
    TrigramIndexReader reader(fixture.root);
    std::string error;
    REQUIRE(!reader.open(fixture_identity(), &error));
    REQUIRE(!error.empty());
}

void test_truncated_postings_fail_on_query() {
    const Fixture fixture = build_fixture();
    std::error_code error_code;
    const auto path = fixture.root / "search.tri";
    const auto size = std::filesystem::file_size(path, error_code);
    REQUIRE(!error_code);
    REQUIRE(size > 1U);
    std::filesystem::resize_file(path, size - 1U, error_code);
    REQUIRE(!error_code);
    TrigramIndexReader reader(fixture.root);
    std::string error;
    // Extent mismatch is detected at open, before any result can escape.
    REQUIRE(!reader.open(fixture_identity(), &error));
    REQUIRE(!error.empty());
}

} // namespace

int main() {
    test_equivalence_no_false_negatives();
    test_short_query_visits_all_blocks();
    test_cancellation_is_observed();
    test_external_sort_merge_path();
    test_corrupt_selected_posting_fails_before_candidates_escape();
    test_corrupt_bloom_fails_before_candidate_escape();
    test_source_identity_mismatch_fails_closed();
    test_corrupt_header_fails_closed();
    test_truncated_postings_fail_on_query();
    std::cout << "Zevryon MassiveDoc trigram index tests passed\n";
    return 0;
}
