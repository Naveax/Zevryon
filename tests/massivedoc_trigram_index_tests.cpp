#include "massivedoc_trigram_index.hpp"

#include <array>
#include <cassert>
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

std::span<const std::byte> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
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
            assert(writer.begin_record(static_cast<std::uint64_t>(block), &error));
            // Force feed boundaries through trigrams.
            const auto bytes = bytes_of(record);
            std::size_t offset = 0U;
            const std::array<std::size_t, 4> pattern{1U, 2U, 3U, 5U};
            std::size_t turn = 0U;
            while (offset < bytes.size()) {
                const std::size_t amount = std::min(pattern[turn % pattern.size()], bytes.size() - offset);
                assert(writer.feed(bytes.subspan(offset, amount), &error));
                offset += amount;
                ++turn;
            }
            assert(writer.end_record(&error));
        }
    }
    TrigramIndexStats stats;
    assert(writer.finish(
        static_cast<std::uint64_t>(fixture.blocks.size()),
        &stats,
        [] { return false; },
        &error));
    assert(stats.block_count == fixture.blocks.size());
    assert(stats.distinct_trigrams != 0U);
    assert(stats.posting_pairs != 0U);
    assert(stats.bloom_bytes == fixture.blocks.size() * kTrigramBloomBytes);
    assert(stats.peak_sort_entries != 0U);
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
    assert(ok);
    return result;
}

void test_equivalence_no_false_negatives() {
    const Fixture fixture = build_fixture();
    TrigramIndexConfig config;
    config.io_window_bytes = 17U;
    TrigramIndexReader reader(fixture.root, config);
    std::string error;
    assert(reader.open(&error));
    assert(reader.block_count() == fixture.blocks.size());

    const std::array<std::string_view, 10> queries{
        "needle", "alpha", "quick brown", "abc", "7890", "middle",
        "record-boundary", "nothing", "does-not-exist", "dle-prefix"};
    for (const auto query : queries) {
        const auto exact = exact_blocks(fixture, query);
        const auto candidates = candidate_blocks(&reader, query);
        for (const auto block : exact) {
            assert(candidates.contains(block));
        }
    }

    // Trigrams must never bridge records. This query exists only if the end of one record
    // is concatenated with the beginning of the next record in block 4.
    const auto cross_record = candidate_blocks(&reader, "needle-prefix");
    assert(!cross_record.contains(4U));

    TrigramQueryStats stats;
    const auto needle_candidates = candidate_blocks(&reader, "needle", &stats);
    assert(needle_candidates.contains(0U));
    assert(needle_candidates.contains(2U));
    assert(stats.posting_streams_opened >= 1U);
    assert(stats.posting_streams_opened <= kTrigramIntersectionStreams);
}

void test_short_query_visits_all_blocks() {
    const Fixture fixture = build_fixture();
    TrigramIndexReader reader(fixture.root);
    std::string error;
    assert(reader.open(&error));
    const auto candidates = candidate_blocks(&reader, "ab");
    assert(candidates.size() == fixture.blocks.size());
}

void test_cancellation_is_observed() {
    const Fixture fixture = build_fixture();
    TrigramIndexReader reader(fixture.root);
    std::string error;
    assert(reader.open(&error));
    int checks = 0;
    TrigramQueryStats stats;
    const bool ok = reader.visit_candidate_blocks(
        "needle",
        [](std::uint64_t) { return true; },
        [&checks] { return ++checks >= 1; },
        &stats,
        &error);
    assert(!ok);
    assert(stats.cancelled);
    assert(error == "trigram query cancelled");
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
        payload[index] = static_cast<char>(
            static_cast<unsigned int>('a') +
            static_cast<unsigned int>((index * 17U + index / 13U) % 26U));
    }
    payload.replace(21000U, 12U, "merge-needle");
    assert(writer.begin_record(0U, &error));
    const auto bytes = bytes_of(payload);
    for (std::size_t offset = 0U; offset < bytes.size();) {
        const std::size_t amount = std::min<std::size_t>(37U, bytes.size() - offset);
        assert(writer.feed(bytes.subspan(offset, amount), &error));
        offset += amount;
    }
    assert(writer.end_record(&error));
    TrigramIndexStats stats;
    assert(writer.finish(1U, &stats, [] { return false; }, &error));
    assert(stats.spool_bytes > 64U * 1024U);
    assert(stats.merge_passes >= 1U);
    assert(stats.peak_sort_entries <= (64U * 1024U) / 16U);

    TrigramIndexReader reader(root, config);
    assert(reader.open(&error));
    const auto candidates = candidate_blocks(&reader, "merge-needle");
    assert(candidates == std::set<std::uint64_t>{0U});
}

void test_corrupt_header_fails_closed() {
    const Fixture fixture = build_fixture();
    {
        std::fstream stream(fixture.root / "search.tri", std::ios::binary | std::ios::in | std::ios::out);
        assert(stream);
        const char bad = 'X';
        stream.write(&bad, 1);
        assert(stream);
    }
    TrigramIndexReader reader(fixture.root);
    std::string error;
    assert(!reader.open(&error));
    assert(!error.empty());
}

void test_truncated_postings_fail_on_query() {
    const Fixture fixture = build_fixture();
    std::error_code error_code;
    const auto path = fixture.root / "search.tri";
    const auto size = std::filesystem::file_size(path, error_code);
    assert(!error_code);
    assert(size > 1U);
    std::filesystem::resize_file(path, size - 1U, error_code);
    assert(!error_code);
    TrigramIndexReader reader(fixture.root);
    std::string error;
    // Extent mismatch is detected at open, before any result can escape.
    assert(!reader.open(&error));
    assert(!error.empty());
}

} // namespace

int main() {
    test_equivalence_no_false_negatives();
    test_short_query_visits_all_blocks();
    test_cancellation_is_observed();
    test_external_sort_merge_path();
    test_corrupt_header_fails_closed();
    test_truncated_postings_fail_on_query();
    std::cout << "Zevryon MassiveDoc trigram index tests passed\n";
    return 0;
}
