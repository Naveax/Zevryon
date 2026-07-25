#include "glyph_cluster_map.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

namespace {
using namespace zevryon::core;
using namespace zevryon::text;

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

ShapedGlyph glyph(std::uint32_t glyph_id, std::uint32_t cluster) {
    return ShapedGlyph{glyph_id, cluster, 100, 0, 0, 0, 0U};
}

MultiRunShapedText make_valid_text() {
    MultiRunShapedText text;
    text.segments.emplace_back(text.glyph_resource());
    MultiRunShapedSegment& ltr = text.segments.back();
    ltr.run = ShapingRunBoundary{
        0U,
        0U,
        ScriptId::Latn,
        ShapingDirection::LeftToRight,
        FontFallbackSource::Primary,
        0U,
        0U};
    ltr.glyphs.first_cluster = 0U;
    ltr.glyphs.cluster_limit = 4U;
    ltr.glyphs.script = ScriptId::Latn;
    ltr.glyphs.direction = ShapingDirection::LeftToRight;
    ltr.glyphs.glyphs.push_back(glyph(10U, 0U));
    ltr.glyphs.glyphs.push_back(glyph(20U, 2U));
    ltr.glyphs.glyphs.push_back(glyph(21U, 2U));
    ltr.glyphs.glyphs.push_back(glyph(30U, 3U));

    text.segments.emplace_back(text.glyph_resource());
    MultiRunShapedSegment& rtl = text.segments.back();
    rtl.run = ShapingRunBoundary{
        4U,
        1U,
        ScriptId::Arab,
        ShapingDirection::RightToLeft,
        FontFallbackSource::ScriptMatch,
        1U,
        0U};
    rtl.glyphs.first_cluster = 4U;
    rtl.glyphs.cluster_limit = 8U;
    rtl.glyphs.script = ScriptId::Arab;
    rtl.glyphs.direction = ShapingDirection::RightToLeft;
    rtl.glyphs.glyphs.push_back(glyph(70U, 7U));
    rtl.glyphs.glyphs.push_back(glyph(60U, 6U));
    rtl.glyphs.glyphs.push_back(glyph(40U, 4U));
    rtl.glyphs.glyphs.push_back(glyph(41U, 4U));
    return text;
}

bool test_exact_ltr_rtl_map() {
    MultiRunShapedText text = make_valid_text();
    GlyphClusterMap map;
    GlyphClusterMapStats stats;
    GlyphClusterMapError error;
    if (!require(
            build_glyph_cluster_map(text, 8U, &map, &stats, &error),
            "valid LTR and RTL shaped text maps")) {
        return false;
    }

    const std::array expected{
        GlyphClusterRecord{0U, 0U, 0U, 1U},
        GlyphClusterRecord{0U, 0U, 0U, 1U},
        GlyphClusterRecord{0U, 2U, 1U, 2U},
        GlyphClusterRecord{0U, 3U, 3U, 1U},
        GlyphClusterRecord{1U, 4U, 2U, 2U},
        GlyphClusterRecord{1U, 4U, 2U, 2U},
        GlyphClusterRecord{1U, 6U, 1U, 1U},
        GlyphClusterRecord{1U, 7U, 0U, 1U}};
    return require(
               std::equal(
                   map.records.begin(),
                   map.records.end(),
                   expected.begin(),
                   expected.end()),
               "logical cluster records are exact") &&
           require(stats.input_segments == 2U, "two segments counted") &&
           require(stats.input_glyphs == 8U, "eight glyphs counted") &&
           require(stats.input_clusters == 8U, "eight clusters counted") &&
           require(stats.output_records == 8U, "one record per cluster") &&
           require(stats.owner_clusters == 6U, "six owner clusters") &&
           require(stats.continuation_clusters == 2U, "two continuation clusters") &&
           require(stats.left_to_right_segments == 1U, "one LTR segment") &&
           require(stats.right_to_left_segments == 1U, "one RTL segment") &&
           require(stats.maximum_group_glyphs == 2U, "maximum glyph group is two") &&
           require(stats.maximum_owner_span_clusters == 2U, "maximum owner span is two");
}

bool test_nonmonotone_failure_clears_output() {
    MultiRunShapedText text = make_valid_text();
    text.segments[0].glyphs.glyphs[2].cluster_index = 1U;
    GlyphClusterMap map;
    map.records.push_back(GlyphClusterRecord{});
    GlyphClusterMapStats stats;
    GlyphClusterMapError error;
    return require(
               !build_glyph_cluster_map(text, 8U, &map, &stats, &error),
               "non-monotone LTR cluster order is rejected") &&
           require(
               error.kind == GlyphClusterMapErrorKind::NonMonotoneGlyphClusters,
               "non-monotone failure is classified") &&
           require(error.segment_index == 0U && error.glyph_index == 2U,
                   "non-monotone location is exact") &&
           require(map.records.empty(), "failure clears stale output");
}

bool test_topology_failure() {
    MultiRunShapedText text = make_valid_text();
    text.segments[1].glyphs.first_cluster = 5U;
    GlyphClusterMap map;
    GlyphClusterMapStats stats;
    GlyphClusterMapError error;
    return require(
               !build_glyph_cluster_map(text, 8U, &map, &stats, &error),
               "segment gap is rejected") &&
           require(
               error.kind == GlyphClusterMapErrorKind::SegmentTopologyViolation,
               "segment gap classification is exact") &&
           require(map.records.empty(), "topology failure publishes no map");
}

bool test_hard_budget() {
    MultiRunShapedText text = make_valid_text();
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::GlyphClusterMap, 1U);
    LedgerMemoryResource memory(ledger, ResourceClass::GlyphClusterMap);
    GlyphClusterMap map(&memory);
    GlyphClusterMapStats stats;
    GlyphClusterMapError error;
    return require(
               !build_glyph_cluster_map(text, 8U, &map, &stats, &error),
               "one-byte hard cap rejects exact map") &&
           require(
               error.kind == GlyphClusterMapErrorKind::OutputBudgetExceeded,
               "hard-cap failure is classified") &&
           require(map.records.empty(), "budget failure publishes no output") &&
           require(
               ledger.snapshot(ResourceClass::GlyphClusterMap)
                       .rejected_reservations == 1U,
               "ledger records rejected allocation") &&
           require(ledger.accounting_clean(), "budget failure keeps accounting clean");
}

} // namespace

int main() {
    if (!test_exact_ltr_rtl_map() ||
        !test_nonmonotone_failure_clears_output() ||
        !test_topology_failure() ||
        !test_hard_budget()) {
        return 1;
    }
    std::cout << "Glyph-cluster map tests passed\n";
    return 0;
}
