#include "grapheme_segmenter.hpp"
#include "ledger_memory_resource.hpp"
#include "unicode_grapheme_data.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <string>
#include <vector>

namespace {

using zevryon::text::DecodedCodePoint;
using zevryon::text::GraphemeBoundary;
using zevryon::text::GraphemeBreakClass;
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

std::uint8_t utf8_length(std::uint32_t value) noexcept {
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
    std::vector<GraphemeBoundary>* result,
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
    std::pmr::vector<GraphemeBoundary> boundaries(&memory);
    if (!zevryon::text::segment_graphemes(
            input,
            &boundaries,
            stats,
            error)) {
        return false;
    }
    result->assign(boundaries.begin(), boundaries.end());
    return true;
}

bool expect_cluster_counts(
    const std::vector<DecodedCodePoint>& input,
    std::initializer_list<std::uint32_t> expected_counts,
    const std::string& label) {
    std::vector<GraphemeBoundary> boundaries;
    GraphemeSegmentStats stats;
    GraphemeError error;
    if (!require(segment(input, &boundaries, &stats, &error), label + " segments") ||
        !require(
            boundaries.size() == expected_counts.size() + 1U,
            label + " boundary count") ||
        !require(
            stats.output_clusters == expected_counts.size(),
            label + " stats cluster count")) {
        return false;
    }

    std::size_t index = 0U;
    for (const std::uint32_t expected : expected_counts) {
        const std::uint32_t actual =
            boundaries[index + 1U].codepoint_index -
            boundaries[index].codepoint_index;
        if (!require(actual == expected, label + " codepoint count")) {
            return false;
        }
        ++index;
    }
    if (!input.empty()) {
        return require(
                   boundaries.front().source_offset == input.front().source_start,
                   label + " source start") &&
               require(
                   boundaries.back().source_offset == input.back().source_end(),
                   label + " final sentinel source") &&
               require(
                   boundaries.back().codepoint_index == input.size(),
                   label + " final sentinel index");
    }
    return boundaries.empty();
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

    return require(sizeof(GraphemeBoundary) <= 16U, "boundary record stays within 16 bytes") &&
           require(cr.break_class == GraphemeBreakClass::CR, "CR property") &&
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

bool test_rules() {
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
               "GB999 ordinary break") &&
           expect_cluster_counts(
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
               "GB12-GB13 odd RI tail") &&
           expect_cluster_counts(
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

bool test_boundaries_and_stats() {
    const auto input =
        make_codepoints({0x1f469U, 0x200dU, 0x1f680U, 'x'}, 1000U);
    std::vector<GraphemeBoundary> boundaries;
    GraphemeSegmentStats stats;
    GraphemeError error;
    if (!require(segment(input, &boundaries, &stats, &error), "range fixture segments") ||
        !require(boundaries.size() == 3U, "two clusters plus sentinel") ||
        !require(boundaries[0].source_offset == 1000U, "first boundary source") ||
        !require(boundaries[0].codepoint_index == 0U, "first boundary index") ||
        !require(boundaries[1].source_offset == input[3].source_start, "second boundary source") ||
        !require(boundaries[1].codepoint_index == 3U, "second boundary index") ||
        !require(boundaries[2].source_offset == input.back().source_end(), "sentinel source") ||
        !require(boundaries[2].codepoint_index == 4U, "sentinel index") ||
        !require(stats.input_codepoints == 4U, "stats input count") ||
        !require(stats.output_clusters == 2U, "stats output count") ||
        !require(stats.suppressed_breaks == 2U, "stats suppressed breaks") ||
        !require(stats.maximum_cluster_codepoints == 3U, "stats maximum codepoints") ||
        !require(
            stats.maximum_cluster_source_bytes ==
                boundaries[1].source_offset - boundaries[0].source_offset,
            "stats maximum bytes")) {
        return false;
    }

    boundaries.clear();
    stats = {};
    error = {};
    const std::vector<DecodedCodePoint> empty;
    return require(segment(empty, &boundaries, &stats, &error), "empty input succeeds") &&
           require(boundaries.empty(), "empty input emits no boundaries");
}

bool test_invalid_input() {
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(
        zevryon::core::ResourceClass::GraphemeCluster,
        4096U);
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::GraphemeCluster);
    std::pmr::vector<GraphemeBoundary> boundaries(&memory);
    GraphemeSegmentStats stats;
    GraphemeError error;

    auto discontinuous = make_codepoints({'a', 'b'});
    discontinuous[1].source_start += 1U;
    if (!require(
            !zevryon::text::segment_graphemes(
                discontinuous,
                &boundaries,
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
                   &boundaries,
                   &stats,
                   &error),
               "zero source length rejected") &&
           require(error.kind == GraphemeErrorKind::InvalidInput, "length error kind");
}

bool test_budget() {
    const auto input = make_codepoints({'a', 'b', 'c'});
    zevryon::core::ResourceLedger rejected;
    rejected.set_hard_limit(
        zevryon::core::ResourceClass::GraphemeCluster,
        1U);
    GraphemeError error;
    GraphemeSegmentStats stats;
    {
        zevryon::core::LedgerMemoryResource memory(
            rejected,
            zevryon::core::ResourceClass::GraphemeCluster);
        std::pmr::vector<GraphemeBoundary> boundaries(&memory);
        if (!require(
                !zevryon::text::segment_graphemes(
                    input,
                    &boundaries,
                    &stats,
                    &error),
                "grapheme hard cap rejects output") ||
            !require(
                error.kind == GraphemeErrorKind::OutputBudgetExceeded,
                "grapheme budget error kind") ||
            !require(
                rejected.snapshot(zevryon::core::ResourceClass::GraphemeCluster)
                        .rejected_reservations >= 1U,
                "grapheme rejected allocation recorded") ||
            !require(
                rejected.snapshot(zevryon::core::ResourceClass::GraphemeCluster)
                        .current_bytes == 0U,
                "rejected allocation consumes no budget")) {
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
        std::pmr::vector<GraphemeBoundary> boundaries(&memory);
        if (!require(
                zevryon::text::segment_graphemes(
                    input,
                    &boundaries,
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
        !test_rules() ||
        !test_boundaries_and_stats() ||
        !test_invalid_input() ||
        !test_budget()) {
        return 1;
    }
    std::cout << "Grapheme segmenter tests passed\n";
    return 0;
}
