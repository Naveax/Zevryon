#include "grapheme_segmenter.hpp"
#include "ledger_memory_resource.hpp"
#include "unicode_grapheme_data.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace {

using zevryon::text::DecodedCodePoint;
using zevryon::text::GraphemeBreakClass;
using zevryon::text::GraphemeCluster;
using zevryon::text::GraphemeError;
using zevryon::text::GraphemeErrorKind;
using zevryon::text::GraphemeSegmentStats;
using zevryon::text::IndicConjunctBreak;

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

std::uint8_t utf8_length(std::uint32_t value) {
    return value <= 0x7fU ? 1U
         : value <= 0x7ffU ? 2U
         : value <= 0xffffU ? 3U
         : 4U;
}

std::vector<DecodedCodePoint> make_codepoints(
    std::initializer_list<std::uint32_t> values,
    std::uint64_t source_base = 0U) {
    std::vector<DecodedCodePoint> output;
    output.reserve(values.size());
    std::uint64_t source = source_base;
    for (const std::uint32_t value : values) {
        const std::uint8_t length = utf8_length(value);
        output.emplace_back(value, source, source + length, false);
        source += length;
    }
    return output;
}

bool segment(
    const std::vector<DecodedCodePoint>& input,
    std::vector<GraphemeCluster>* result,
    GraphemeSegmentStats* stats,
    GraphemeError* error,
    std::size_t budget = 1U << 20U) {
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(
        zevryon::core::ResourceClass::GraphemeCluster,
        budget);
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::GraphemeCluster);
    std::pmr::vector<GraphemeCluster> clusters(&memory);
    if (!zevryon::text::segment_graphemes(input, &clusters, stats, error)) {
        return false;
    }
    result->assign(clusters.begin(), clusters.end());
    return true;
}

bool expect_cluster_counts(
    const std::vector<DecodedCodePoint>& input,
    std::initializer_list<std::uint32_t> expected_counts,
    const std::string& label) {
    std::vector<GraphemeCluster> clusters;
    GraphemeSegmentStats stats;
    GraphemeError error;
    if (!require(segment(input, &clusters, &stats, &error), label + " segments") ||
        !require(clusters.size() == expected_counts.size(), label + " cluster count")) {
        return false;
    }
    std::size_t index = 0U;
    for (const std::uint32_t expected : expected_counts) {
        if (!require(
                clusters[index].codepoint_count == expected,
                label + " codepoint count")) {
            return false;
        }
        ++index;
    }
    if (!input.empty()) {
        if (!require(clusters.front().source_start == input.front().source_start, label + " source start") ||
            !require(clusters.back().source_end() == input.back().source_end(), label + " source end")) {
            return false;
        }
    }
    return true;
}

bool test_property_samples() {
    const auto cr = zevryon::text::grapheme_properties(0x000dU);
    const auto lf = zevryon::text::grapheme_properties(0x000aU);
    const auto control = zevryon::text::grapheme_properties(0x0000U);
    const auto extend = zevryon::text::grapheme_properties(0x0301U);
    const auto zwj = zevryon::text::grapheme_properties(0x200dU);
    const auto ri = zevryon::text::grapheme_properties(0x1f1e6U);
    const auto prepend = zevryon::text::grapheme_properties(0x0600U);
    const auto spacing = zevryon::text::grapheme_properties(0x0903U);
    const auto l = zevryon::text::grapheme_properties(0x1100U);
    const auto v = zevryon::text::grapheme_properties(0x1161U);
    const auto t = zevryon::text::grapheme_properties(0x11a8U);
    const auto lv = zevryon::text::grapheme_properties(0xac00U);
    const auto lvt = zevryon::text::grapheme_properties(0xac01U);
    const auto emoji = zevryon::text::grapheme_properties(0x1f469U);
    const auto consonant = zevryon::text::grapheme_properties(0x0915U);
    const auto linker = zevryon::text::grapheme_properties(0x094dU);

    return require(cr.break_class == GraphemeBreakClass::CR, "CR property") &&
           require(lf.break_class == GraphemeBreakClass::LF, "LF property") &&
           require(control.break_class == GraphemeBreakClass::Control, "Control property") &&
           require(extend.break_class == GraphemeBreakClass::Extend, "Extend property") &&
           require(zwj.break_class == GraphemeBreakClass::ZWJ, "ZWJ property") &&
           require(ri.break_class == GraphemeBreakClass::RegionalIndicator, "RI property") &&
           require(prepend.break_class == GraphemeBreakClass::Prepend, "Prepend property") &&
           require(spacing.break_class == GraphemeBreakClass::SpacingMark, "SpacingMark property") &&
           require(l.break_class == GraphemeBreakClass::L, "Hangul L property") &&
           require(v.break_class == GraphemeBreakClass::V, "Hangul V property") &&
           require(t.break_class == GraphemeBreakClass::T, "Hangul T property") &&
           require(lv.break_class == GraphemeBreakClass::LV, "Hangul LV algorithm") &&
           require(lvt.break_class == GraphemeBreakClass::LVT, "Hangul LVT algorithm") &&
           require(emoji.extended_pictographic, "Extended pictographic property") &&
           require(
               consonant.indic_conjunct_break == IndicConjunctBreak::Consonant,
               "Indic consonant property") &&
           require(
               linker.indic_conjunct_break == IndicConjunctBreak::Linker,
               "Indic linker property") &&
           require(
               extend.indic_conjunct_break == IndicConjunctBreak::Extend,
               "Indic extend property") &&
           require(
               std::string(zevryon::text::kUnicodeGraphemeDataVersion) == "17.0.0",
               "Unicode data version");
}

bool test_core_rules() {
    return expect_cluster_counts(
               make_codepoints({'a', 0x0301U}),
               {2U},
               "GB9 combining mark") &&
           expect_cluster_counts(
               make_codepoints({'a', 0x000dU, 0x000aU, 'b'}),
               {1U, 2U, 1U},
               "GB3 CRLF and controls") &&
           expect_cluster_counts(
               make_codepoints({0x1100U, 0x1161U, 0x11a8U}),
               {3U},
               "GB6-GB8 Hangul") &&
           expect_cluster_counts(
               make_codepoints({0x0600U, 'a'}),
               {2U},
               "GB9b prepend") &&
           expect_cluster_counts(
               make_codepoints({'a', 0x0903U}),
               {2U},
               "GB9a spacing mark") &&
           expect_cluster_counts(
               make_codepoints({'a', 'b'}),
               {1U, 1U},
               "GB999 ordinary break");
}

bool test_emoji_and_flags() {
    return expect_cluster_counts(
               make_codepoints({0x1f469U, 0x200dU, 0x1f680U}),
               {3U},
               "GB11 emoji ZWJ") &&
           expect_cluster_counts(
               make_codepoints({0x1f469U, 0x0301U, 0x200dU, 0x1f680U}),
               {4U},
               "GB11 emoji Extend ZWJ") &&
           expect_cluster_counts(
               make_codepoints({0x1f1e6U, 0x1f1e7U, 0x1f1e8U, 0x1f1e9U}),
               {2U, 2U},
               "GB12-GB13 RI parity") &&
           expect_cluster_counts(
               make_codepoints({0x1f1e6U, 0x1f1e7U, 0x1f1e8U}),
               {2U, 1U},
               "GB12-GB13 odd RI tail");
}

bool test_indic_conjunct() {
    return expect_cluster_counts(
               make_codepoints({0x0915U, 0x094dU, 0x0915U}),
               {3U},
               "GB9c Devanagari conjunct") &&
           expect_cluster_counts(
               make_codepoints({0x0915U, 0x094dU, 0x0301U, 0x0915U}),
               {4U},
               "GB9c linker with Extend") &&
           expect_cluster_counts(
               make_codepoints({0x0915U, 0x0301U, 0x0915U}),
               {2U, 1U},
               "GB9c requires linker");
}

bool test_ranges_and_stats() {
    const auto input = make_codepoints({0x1f469U, 0x200dU, 0x1f680U, 'x'}, 1000U);
    std::vector<GraphemeCluster> clusters;
    GraphemeSegmentStats stats;
    GraphemeError error;
    if (!require(segment(input, &clusters, &stats, &error), "range fixture segments") ||
        !require(clusters.size() == 2U, "range fixture cluster count") ||
        !require(clusters[0].source_start == 1000U, "first cluster source start") ||
        !require(clusters[0].source_end() == input[2].source_end(), "first cluster source end") ||
        !require(clusters[0].first_codepoint == 0U, "first cluster index") ||
        !require(clusters[0].codepoint_count == 3U, "first cluster count") ||
        !require(clusters[1].first_codepoint == 3U, "second cluster index") ||
        !require(stats.input_codepoints == 4U, "stats input count") ||
        !require(stats.output_clusters == 2U, "stats output count") ||
        !require(stats.suppressed_breaks == 2U, "stats suppressed breaks") ||
        !require(stats.maximum_cluster_codepoints == 3U, "stats maximum codepoints") ||
        !require(stats.maximum_cluster_source_bytes == clusters[0].source_length, "stats maximum bytes")) {
        return false;
    }

    clusters.clear();
    stats = {};
    error = {};
    const std::vector<DecodedCodePoint> empty;
    return require(segment(empty, &clusters, &stats, &error), "empty input succeeds") &&
           require(clusters.empty(), "empty input emits no clusters");
}

bool test_invalid_input() {
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(zevryon::core::ResourceClass::GraphemeCluster, 4096U);
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::GraphemeCluster);
    std::pmr::vector<GraphemeCluster> clusters(&memory);
    GraphemeSegmentStats stats;
    GraphemeError error;

    auto discontinuous = make_codepoints({'a', 'b'});
    discontinuous[1].source_start += 1U;
    if (!require(
            !zevryon::text::segment_graphemes(
                discontinuous,
                &clusters,
                &stats,
                &error),
            "discontinuous ranges rejected") ||
        !require(error.kind == GraphemeErrorKind::InvalidInput, "discontinuous error kind") ||
        !require(error.codepoint_index == 1U, "discontinuous error index")) {
        return false;
    }

    auto invalid_length = make_codepoints({'a'});
    invalid_length[0].source_length = 0U;
    return require(
               !zevryon::text::segment_graphemes(
                   invalid_length,
                   &clusters,
                   &stats,
                   &error),
               "zero source length rejected") &&
           require(error.kind == GraphemeErrorKind::InvalidInput, "length error kind");
}

bool test_budget() {
    const auto input = make_codepoints({'a', 'b', 'c'});
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(zevryon::core::ResourceClass::GraphemeCluster, 1U);
    GraphemeError error;
    GraphemeSegmentStats stats;
    {
        zevryon::core::LedgerMemoryResource memory(
            ledger,
            zevryon::core::ResourceClass::GraphemeCluster);
        std::pmr::vector<GraphemeCluster> clusters(&memory);
        if (!require(
                !zevryon::text::segment_graphemes(
                    input,
                    &clusters,
                    &stats,
                    &error),
                "grapheme hard cap rejects output") ||
            !require(
                error.kind == GraphemeErrorKind::OutputBudgetExceeded,
                "grapheme budget error kind") ||
            !require(
                ledger.snapshot(zevryon::core::ResourceClass::GraphemeCluster)
                        .rejected_reservations >= 1U,
                "grapheme rejected allocation recorded") ||
            !require(
                ledger.snapshot(zevryon::core::ResourceClass::GraphemeCluster)
                        .current_bytes == 0U,
                "rejected grapheme allocation consumes no budget")) {
            return false;
        }
    }

    zevryon::core::ResourceLedger released;
    released.set_hard_limit(
        zevryon::core::ResourceClass::GraphemeCluster,
        4096U);
    {
        zevryon::core::LedgerMemoryResource memory(
            released,
            zevryon::core::ResourceClass::GraphemeCluster);
        std::pmr::vector<GraphemeCluster> clusters(&memory);
        if (!require(
                zevryon::text::segment_graphemes(
                    input,
                    &clusters,
                    &stats,
                    &error),
                "budgeted grapheme output succeeds") ||
            !require(
                released.snapshot(zevryon::core::ResourceClass::GraphemeCluster)
                        .current_bytes > 0U,
                "grapheme allocation charged")) {
            return false;
        }
    }
    return require(
               released.snapshot(zevryon::core::ResourceClass::GraphemeCluster)
                       .current_bytes == 0U,
               "grapheme PMR releases allocation") &&
           require(released.accounting_clean(), "grapheme accounting clean");
}

} // namespace

int main() {
    if (!test_property_samples() ||
        !test_core_rules() ||
        !test_emoji_and_flags() ||
        !test_indic_conjunct() ||
        !test_ranges_and_stats() ||
        !test_invalid_input() ||
        !test_budget()) {
        return 1;
    }
    std::cout << "Grapheme segmenter tests passed\n";
    return 0;
}
