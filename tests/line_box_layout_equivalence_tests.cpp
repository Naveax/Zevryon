#include "line_box_layout.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

using namespace zevryon::text;

std::int64_t scale_reference(
    std::int32_t value,
    std::int32_t scale,
    std::uint32_t units_per_em) {
    const std::int64_t widened = value;
    const bool negative = widened < 0;
    const std::uint64_t magnitude = static_cast<std::uint64_t>(
        negative ? -widened : widened);
    const std::uint64_t product =
        magnitude * static_cast<std::uint32_t>(scale);
    const std::uint64_t rounded =
        (product + units_per_em / 2U) / units_per_em;
    const std::int64_t result = static_cast<std::int64_t>(rounded);
    return negative ? -result : result;
}

} // namespace

int main() {
    constexpr std::array<std::int32_t, 4U> ascenders{400, 700, 800, 1000};
    constexpr std::array<std::int32_t, 4U> descenders{0, -100, -250, -400};
    constexpr std::array<std::int32_t, 7U> gaps{-100, -1, 0, 1, 99, 100, 301};
    constexpr std::array<std::uint32_t, 2U> upems{1000U, 2048U};
    constexpr std::array<std::int32_t, 5U> scales{1, 333, 1000, 2048, 4096};

    FontLineMetricTable metrics;
    metrics.records.resize(1U);
    MultiRunShapedText shaped;
    shaped.segments.emplace_back(shaped.glyph_resource());
    MultiRunShapedSegment& segment = shaped.segments.back();
    segment.run = ShapingRunBoundary{
        0U,
        1U,
        ScriptId::Latn,
        ShapingDirection::LeftToRight,
        FontFallbackSource::Primary,
        0U,
        0U};
    segment.glyphs.first_cluster = 0U;
    segment.glyphs.cluster_limit = 1U;
    segment.glyphs.script = ScriptId::Latn;
    segment.glyphs.direction = ShapingDirection::LeftToRight;

    LineFragmentLayout fragments;
    fragments.lines.push_back(VisualLineLayoutRecord{1U, 0U, 1U, 1U, 0U});
    fragments.fragments.push_back(InlineLayoutFragment{
        0U, 1U, 0U, 0U, 1U, 0U, 0U, 0U});

    LineBoxLayout output;
    std::uint64_t cases = 0U;
    for (const std::int32_t ascender : ascenders) {
        for (const std::int32_t descender : descenders) {
            for (const std::int32_t gap : gaps) {
                for (const std::uint32_t upem : upems) {
                    for (const std::int32_t scale : scales) {
                        if (static_cast<std::int64_t>(ascender) - descender + gap <= 0) {
                            continue;
                        }
                        std::uint8_t flags = static_cast<std::uint8_t>(
                            kFontLineMetricHasOs2 |
                            kFontLineMetricUseTypoMetrics);
                        if (gap < 0) {
                            flags = static_cast<std::uint8_t>(
                                flags | kFontLineMetricNegativeLineGap);
                        }
                        metrics.records[0] = FontLineMetricRecord{
                            1U,
                            upem,
                            ascender,
                            descender,
                            gap,
                            static_cast<std::uint32_t>(ascender),
                            static_cast<std::uint32_t>(-descender),
                            FontLineMetricSource::Os2Typographic,
                            flags,
                            0U};
                        segment.glyphs.x_scale = scale;
                        segment.glyphs.y_scale = scale;

                        const std::int64_t scaled_ascender =
                            scale_reference(ascender, scale, upem);
                        const std::int64_t scaled_descender =
                            scale_reference(descender, scale, upem);
                        const std::int64_t scaled_gap =
                            scale_reference(gap, scale, upem);
                        const std::int64_t before = scaled_gap / 2;
                        const std::int64_t after = scaled_gap - before;
                        const std::int64_t expected_ascent =
                            scaled_ascender + before;
                        const std::int64_t expected_descent =
                            -scaled_descender + after;

                        LineBoxLayoutStats stats;
                        LineBoxLayoutError error;
                        const bool succeeded = build_line_box_layout(
                            LineBoxLayoutRequest{
                                &fragments,
                                &shaped,
                                &metrics,
                                1U,
                                scale},
                            &output,
                            &stats,
                            &error);
                        const bool representable =
                            expected_ascent >= 0 && expected_descent >= 0 &&
                            expected_ascent + expected_descent > 0;
                        if (succeeded != representable) {
                            std::cerr << "scale equivalence mismatch\n";
                            return EXIT_FAILURE;
                        }
                        if (succeeded) {
                            const std::uint64_t expected_block =
                                static_cast<std::uint64_t>(
                                    expected_ascent + expected_descent);
                            if (output.lines.size() != 1U ||
                                output.fragment_metrics.size() != 1U ||
                                output.lines[0].baseline !=
                                    static_cast<std::uint64_t>(expected_ascent) ||
                                output.lines[0].block_size != expected_block ||
                                output.fragment_metrics[0].baseline_offset !=
                                    static_cast<std::uint64_t>(expected_ascent) ||
                                output.fragment_metrics[0].block_size != expected_block ||
                                output.fragment_metrics[0].block_offset != 0U) {
                                std::cerr << "line-box metric equivalence mismatch\n";
                                return EXIT_FAILURE;
                            }
                        }
                        ++cases;
                    }
                }
            }
        }
    }
    if (cases != 1'120U) {
        std::cerr << "unexpected exhaustive case count: " << cases << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "line box metric equivalence cases=" << cases << '\n';
    return 0;
}
