#include "line_fragment_layout.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using namespace zevryon::text;

[[noreturn]] void fail_case(
    std::size_t length,
    std::uint64_t pattern,
    const char* message) {
    std::cerr << "line fragment equivalence failure: " << message
              << " length=" << length
              << " pattern=" << pattern << '\n';
    std::abort();
}

void verify_case(std::size_t length, std::uint64_t pattern) {
    std::vector<GraphemeBoundary> boundaries;
    std::vector<BidiExplicitUnit> units;
    BidiSequenceTopology topology;
    std::vector<std::uint8_t> levels;
    MultiRunShapedText shaped;
    GlyphClusterMap cluster_map;
    LineSelection selection;

    boundaries.reserve(length + 1U);
    units.reserve(length);
    topology.active_unit_indices.reserve(length);
    levels.reserve(length);
    shaped.segments.reserve(length);
    cluster_map.records.reserve(length);

    std::uint64_t digits = pattern;
    for (std::size_t index = 0U; index <= length; ++index) {
        boundaries.push_back(GraphemeBoundary{
            static_cast<std::uint64_t>(index),
            static_cast<std::uint32_t>(index)});
    }

    for (std::size_t index = 0U; index < length; ++index) {
        const std::uint8_t level = static_cast<std::uint8_t>(digits & 3U);
        digits >>= 2U;
        const std::uint32_t cluster = static_cast<std::uint32_t>(index);
        levels.push_back(level);
        units.push_back(BidiExplicitUnit{
            static_cast<std::uint64_t>(index),
            cluster,
            (level & 1U) == 0U ? BidiClass::L : BidiClass::R,
            (level & 1U) == 0U ? BidiClass::L : BidiClass::R,
            level,
            0U});
        topology.active_unit_indices.push_back(cluster);

        shaped.segments.emplace_back(shaped.glyph_resource());
        MultiRunShapedSegment& segment = shaped.segments.back();
        segment.run = ShapingRunBoundary{
            cluster,
            static_cast<FontFaceId>(1U),
            ScriptId::Latn,
            (level & 1U) == 0U
                ? ShapingDirection::LeftToRight
                : ShapingDirection::RightToLeft,
            FontFallbackSource::Primary,
            level,
            0U};
        segment.glyphs.first_cluster = cluster;
        segment.glyphs.cluster_limit = cluster + 1U;
        segment.glyphs.script = segment.run.script;
        segment.glyphs.direction = segment.run.direction;
        segment.glyphs.glyphs.push_back(ShapedGlyph{
            100U + cluster,
            cluster,
            10,
            0,
            0,
            0,
            0U});
        cluster_map.records.push_back(GlyphClusterRecord{
            cluster,
            cluster,
            0U,
            1U});
    }

    selection.lines.push_back(SelectedLineRecord{
        static_cast<std::uint64_t>(length) * 10U,
        static_cast<std::uint32_t>(length),
        kSelectedLineMandatoryBreak | kSelectedLineTextEnd});

    BidiVisualOrder expected;
    BidiVisualStats expected_stats;
    BidiVisualError expected_error;
    const BidiLineSpan line{
        0U,
        static_cast<std::uint32_t>(length)};
    if (!resolve_bidi_visual_order(
            units,
            topology,
            levels,
            0U,
            std::span<const BidiLineSpan>(&line, 1U),
            &expected,
            &expected_stats,
            &expected_error)) {
        fail_case(length, pattern, "reference visual-order resolution failed");
    }

    LineFragmentLayout output;
    LineFragmentLayoutStats stats;
    LineFragmentLayoutError error;
    if (!build_line_fragment_layout(
            LineFragmentLayoutRequest{
                boundaries,
                units,
                &topology,
                levels,
                0U,
                &shaped,
                &cluster_map,
                &selection,
                static_cast<std::uint32_t>(length)},
            &output,
            &stats,
            &error)) {
        fail_case(length, pattern, "fragment layout failed");
    }

    if (output.fragments.size() != length ||
        expected.visual_to_active.size() != length) {
        fail_case(length, pattern, "output size mismatch");
    }
    for (std::size_t position = 0U; position < length; ++position) {
        const std::uint32_t expected_cluster =
            expected.visual_to_active[position];
        const InlineLayoutFragment& fragment = output.fragments[position];
        if (fragment.segment_index != expected_cluster ||
            fragment.first_cluster != expected_cluster ||
            fragment.cluster_limit != expected_cluster + 1U ||
            fragment.bidi_level != expected.line_levels[expected_cluster] ||
            fragment.inline_offset != position * 10U ||
            fragment.inline_advance != 10U) {
            fail_case(length, pattern, "fragment/scalar visual order diverged");
        }
    }
    if (stats.l2_reversal_spans != expected_stats.l2_reversal_spans ||
        stats.l2_reversed_fragments != expected_stats.l2_reversed_units) {
        fail_case(length, pattern, "L2 reversal accounting diverged");
    }
}

} // namespace

int main() {
    std::uint64_t cases = 0U;
    for (std::size_t length = 1U; length <= 6U; ++length) {
        const std::uint64_t pattern_count = 1ULL << (2U * length);
        for (std::uint64_t pattern = 0U;
             pattern < pattern_count;
             ++pattern) {
            verify_case(length, pattern);
            ++cases;
        }
    }
    std::cout << "line fragment L2 equivalence passed "
              << cases << " cases\n";
    return EXIT_SUCCESS;
}
