#include "line_selection.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

using namespace zevryon::text;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

} // namespace

int main() {
    MultiRunShapedText shaped_text;
    shaped_text.segments.emplace_back(shaped_text.glyph_resource());
    MultiRunShapedSegment& segment = shaped_text.segments.back();
    segment.run = ShapingRunBoundary{
        0U,
        static_cast<FontFaceId>(3U),
        ScriptId::Latn,
        ShapingDirection::LeftToRight,
        FontFallbackSource::Primary,
        0U,
        0U};
    segment.glyphs.first_cluster = 0U;
    segment.glyphs.cluster_limit = 1U;
    segment.glyphs.script = segment.run.script;
    segment.glyphs.direction = segment.run.direction;
    segment.glyphs.glyphs.push_back(ShapedGlyph{
        301U, 0U, 10, 0, 0, 0, 0U});
    segment.glyphs.glyphs.push_back(ShapedGlyph{
        302U, 0U, -3, 0, 0, 0, 0U});

    GlyphClusterMap cluster_map;
    cluster_map.records.push_back(GlyphClusterRecord{
        0U, 0U, 0U, 2U});

    CaretBoundaryMap caret_map;
    caret_map.flags.push_back(
        static_cast<std::uint8_t>(kCaretBoundarySafe));
    caret_map.flags.push_back(
        static_cast<std::uint8_t>(kCaretBoundarySafe));

    LineBreakOpportunityMap opportunity_map;
    opportunity_map.opportunities.push_back(
        static_cast<std::uint8_t>(LineBreakOpportunity::Prohibited));
    opportunity_map.opportunities.push_back(
        static_cast<std::uint8_t>(LineBreakOpportunity::Mandatory));

    LineSelection selection;
    LineSelectionStats stats;
    LineSelectionError error;
    require(
        select_bounded_lines(
            LineSelectionRequest{
                &shaped_text,
                &cluster_map,
                &caret_map,
                &opportunity_map,
                1U,
                7U},
            &selection,
            &stats,
            &error),
        "mixed-sign glyph group must select successfully");
    require(selection.lines.size() == 1U, "one line expected");
    require(
        selection.lines[0].inline_advance == 7U,
        "group width must use abs(sum(x_advance))");
    require(
        !selected_line_has_flag(
            selection,
            0U,
            kSelectedLineOverflow),
        "net seven-unit group must fit seven-unit width");
    require(stats.total_inline_advance == 7U, "stats must retain net width");

    std::cout << "signed line-selection advance test passed\n";
    return 0;
}
