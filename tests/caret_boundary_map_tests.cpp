#include "caret_boundary_map.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <new>
#include <string>

namespace {
using namespace zevryon::text;

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

ShapedGlyph glyph(
    std::uint32_t glyph_id,
    std::uint32_t cluster,
    std::uint32_t flags = 0U) {
    return ShapedGlyph{glyph_id, cluster, 100, 0, 0, 0, flags};
}

struct Fixture final {
    MultiRunShapedText text;
    GlyphClusterMap cluster_map;

    Fixture() {
        text.segments.emplace_back(text.glyph_resource());
        MultiRunShapedSegment& ltr = text.segments.back();
        ltr.glyphs.first_cluster = 0U;
        ltr.glyphs.cluster_limit = 4U;
        ltr.glyphs.direction = ShapingDirection::LeftToRight;
        ltr.glyphs.glyphs.push_back(glyph(10U, 0U));
        ltr.glyphs.glyphs.push_back(glyph(20U, 2U));
        ltr.glyphs.glyphs.push_back(glyph(21U, 2U));
        ltr.glyphs.glyphs.push_back(glyph(30U, 3U));

        text.segments.emplace_back(text.glyph_resource());
        MultiRunShapedSegment& rtl = text.segments.back();
        rtl.glyphs.first_cluster = 4U;
        rtl.glyphs.cluster_limit = 8U;
        rtl.glyphs.direction = ShapingDirection::RightToLeft;
        rtl.glyphs.glyphs.push_back(glyph(70U, 7U));
        rtl.glyphs.glyphs.push_back(glyph(60U, 6U));
        rtl.glyphs.glyphs.push_back(glyph(40U, 4U));
        rtl.glyphs.glyphs.push_back(glyph(41U, 4U));

        const std::array records{
            GlyphClusterRecord{0U, 0U, 0U, 1U},
            GlyphClusterRecord{0U, 0U, 0U, 1U},
            GlyphClusterRecord{0U, 2U, 1U, 2U},
            GlyphClusterRecord{0U, 3U, 3U, 1U},
            GlyphClusterRecord{1U, 4U, 2U, 2U},
            GlyphClusterRecord{1U, 4U, 2U, 2U},
            GlyphClusterRecord{1U, 6U, 1U, 1U},
            GlyphClusterRecord{1U, 7U, 0U, 1U}};
        cluster_map.records.assign(records.begin(), records.end());
    }
};

class RejectingResource final : public std::pmr::memory_resource {
private:
    void* do_allocate(std::size_t, std::size_t) override {
        throw std::bad_alloc();
    }
    void do_deallocate(void*, std::size_t, std::size_t) override {}
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

bool build(
    const Fixture& fixture,
    CaretBoundaryMap* map,
    CaretBoundaryMapStats* stats,
    CaretBoundaryMapError* error) {
    return build_caret_boundary_map(
        CaretBoundaryMapRequest{&fixture.text, &fixture.cluster_map, 8U},
        map,
        stats,
        error);
}

bool test_exact_boundaries() {
    Fixture fixture;
    CaretBoundaryMap map;
    CaretBoundaryMapStats stats;
    CaretBoundaryMapError error;
    if (!require(build(fixture, &map, &stats, &error), "valid map builds")) {
        return false;
    }

    const auto safe = [&](std::uint32_t boundary) {
        return caret_boundary_has_flag(map, boundary, kCaretBoundarySafe);
    };
    return require(map.flags.size() == 9U, "one byte per logical boundary") &&
           require(safe(0U) && safe(2U) && safe(3U) && safe(4U) &&
                       safe(6U) && safe(7U) && safe(8U),
                   "clean glyph and text edges are safe") &&
           require(!safe(1U) && !safe(5U),
                   "merged-group interiors are unsafe") &&
           require(caret_boundary_has_flag(
                       map, 1U, kCaretBoundaryInsideMergedGroup),
                   "LTR ligature interior is identified") &&
           require(caret_boundary_has_flag(
                       map, 5U, kCaretBoundaryInsideMergedGroup),
                   "RTL merged interior is identified") &&
           require(caret_boundary_has_flag(map, 4U, kCaretBoundaryRunEdge),
                   "logical run edge is identified") &&
           require(stats.input_segments == 2U, "two segments counted") &&
           require(stats.input_glyphs == 8U, "eight glyphs counted") &&
           require(stats.input_clusters == 8U, "eight clusters counted") &&
           require(stats.output_boundaries == 9U, "nine boundaries published") &&
           require(stats.glyph_groups == 6U, "six glyph groups counted") &&
           require(stats.safe_boundaries == 7U, "seven safe boundaries") &&
           require(stats.unsafe_boundaries == 2U, "two unsafe boundaries") &&
           require(stats.text_edge_boundaries == 2U, "two text edges") &&
           require(stats.run_edge_boundaries == 3U, "text and segment run edges") &&
           require(stats.glyph_edge_boundaries == 7U, "seven glyph edges") &&
           require(stats.merged_interior_boundaries == 2U,
                   "two merged interiors") &&
           require(stats.unsafe_to_break_boundaries == 0U,
                   "no HarfBuzz unsafe evidence") &&
           require(!caret_boundary_has_flag(map, 9U, kCaretBoundarySafe),
                   "out-of-range query fails closed");
}

bool test_unsafe_to_break_propagation() {
    Fixture fixture;
    fixture.text.segments[0].glyphs.glyphs[1].flags =
        kShapedGlyphUnsafeToBreak;
    CaretBoundaryMap map;
    CaretBoundaryMapStats stats;
    CaretBoundaryMapError error;
    return require(build(fixture, &map, &stats, &error),
                   "unsafe fixture builds") &&
           require(caret_boundary_has_flag(
                       map, 2U, kCaretBoundaryUnsafeToBreak),
                   "unsafe evidence propagates to left group edge") &&
           require(caret_boundary_has_flag(
                       map, 3U, kCaretBoundaryUnsafeToBreak),
                   "unsafe evidence propagates to right group edge") &&
           require(!caret_boundary_has_flag(map, 2U, kCaretBoundarySafe) &&
                       !caret_boundary_has_flag(map, 3U, kCaretBoundarySafe),
                   "unsafe group edges are not caret-safe") &&
           require(stats.unsafe_to_break_boundaries == 2U,
                   "two unsafe-to-break edges counted") &&
           require(stats.safe_boundaries == 5U &&
                       stats.unsafe_boundaries == 4U,
                   "unsafe totals remain exact");
}

bool test_inconsistent_owner_clears_output() {
    Fixture fixture;
    fixture.cluster_map.records[0].owner_cluster = 1U;
    CaretBoundaryMap map;
    map.flags.push_back(kCaretBoundarySafe);
    CaretBoundaryMapStats stats;
    CaretBoundaryMapError error;
    return require(!build(fixture, &map, &stats, &error),
                   "invalid owner is rejected") &&
           require(error.kind ==
                       CaretBoundaryMapErrorKind::InconsistentClusterMap,
                   "invalid owner classification is exact") &&
           require(map.flags.empty(), "failure clears stale output");
}

bool test_invalid_glyph_span() {
    Fixture fixture;
    fixture.cluster_map.records[2].first_glyph = 100U;
    CaretBoundaryMap map;
    CaretBoundaryMapStats stats;
    CaretBoundaryMapError error;
    return require(!build(fixture, &map, &stats, &error),
                   "invalid glyph span is rejected") &&
           require(error.kind == CaretBoundaryMapErrorKind::InvalidGlyphSpan,
                   "glyph-span classification is exact") &&
           require(map.flags.empty(), "invalid span publishes no output");
}

bool test_unmapped_glyph_rejected() {
    Fixture fixture;
    fixture.text.segments[0].glyphs.glyphs.push_back(glyph(99U, 3U));
    CaretBoundaryMap map;
    CaretBoundaryMapStats stats;
    CaretBoundaryMapError error;
    return require(!build(fixture, &map, &stats, &error),
                   "unmapped retained glyph is rejected") &&
           require(error.kind ==
                       CaretBoundaryMapErrorKind::InconsistentClusterMap,
                   "unmapped-glyph classification is exact") &&
           require(map.flags.empty(), "coverage failure publishes no output");
}

bool test_budget_failure() {
    Fixture fixture;
    RejectingResource resource;
    CaretBoundaryMap map(&resource);
    CaretBoundaryMapStats stats;
    CaretBoundaryMapError error;
    return require(!build(fixture, &map, &stats, &error),
                   "rejected allocation fails") &&
           require(error.kind == CaretBoundaryMapErrorKind::OutputBudgetExceeded,
                   "budget classification is exact") &&
           require(map.flags.empty(), "budget failure publishes no output");
}

bool test_empty_text() {
    MultiRunShapedText text;
    GlyphClusterMap cluster_map;
    CaretBoundaryMap map;
    CaretBoundaryMapStats stats;
    CaretBoundaryMapError error;
    return require(
               build_caret_boundary_map(
                   CaretBoundaryMapRequest{&text, &cluster_map, 0U},
                   &map,
                   &stats,
                   &error),
               "empty text has one caret boundary") &&
           require(map.flags.size() == 1U, "empty text publishes one byte") &&
           require(caret_boundary_has_flag(map, 0U, kCaretBoundarySafe),
                   "empty-text boundary is safe") &&
           require(stats.safe_boundaries == 1U &&
                       stats.text_edge_boundaries == 1U,
                   "empty-text statistics are exact");
}

} // namespace

int main() {
    if (!test_exact_boundaries() ||
        !test_unsafe_to_break_propagation() ||
        !test_inconsistent_owner_clears_output() ||
        !test_invalid_glyph_span() ||
        !test_unmapped_glyph_rejected() ||
        !test_budget_failure() ||
        !test_empty_text()) {
        return 1;
    }
    std::cout << "Caret-boundary map tests passed\n";
    return 0;
}
