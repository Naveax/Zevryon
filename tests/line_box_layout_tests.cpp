#include "line_box_layout.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory_resource>

namespace {

using namespace zevryon::text;

#define REQUIRE(condition)                                                     \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << "requirement failed at line " << __LINE__ << ": "  \
                      << #condition << '\n';                                   \
            std::exit(EXIT_FAILURE);                                           \
        }                                                                      \
    } while (false)

FontLineMetricRecord metric(
    FontFaceId face_id,
    std::uint32_t units_per_em,
    std::int32_t ascender,
    std::int32_t descender,
    std::int32_t line_gap,
    FontLineMetricSource source = FontLineMetricSource::Os2Typographic) {
    std::uint8_t flags = source == FontLineMetricSource::HorizontalHeader
        ? static_cast<std::uint8_t>(kFontLineMetricHasHhea)
        : static_cast<std::uint8_t>(
              kFontLineMetricHasOs2 | kFontLineMetricUseTypoMetrics);
    if (line_gap < 0) {
        flags = static_cast<std::uint8_t>(
            flags | kFontLineMetricNegativeLineGap);
    }
    return FontLineMetricRecord{
        face_id,
        units_per_em,
        ascender,
        descender,
        line_gap,
        static_cast<std::uint32_t>(ascender),
        static_cast<std::uint32_t>(-descender),
        source,
        flags,
        0U};
}

void add_segment(
    MultiRunShapedText* shaped,
    std::uint32_t first_cluster,
    std::uint32_t cluster_limit,
    FontFaceId face_id,
    std::int32_t y_scale,
    ShapingDirection direction = ShapingDirection::LeftToRight) {
    shaped->segments.emplace_back(shaped->glyph_resource());
    MultiRunShapedSegment& segment = shaped->segments.back();
    segment.run = ShapingRunBoundary{
        first_cluster,
        face_id,
        ScriptId::Latn,
        direction,
        FontFallbackSource::Primary,
        static_cast<std::uint8_t>(
            direction == ShapingDirection::RightToLeft ? 1U : 0U),
        0U};
    segment.glyphs.first_cluster = first_cluster;
    segment.glyphs.cluster_limit = cluster_limit;
    segment.glyphs.script = ScriptId::Latn;
    segment.glyphs.direction = direction;
    segment.glyphs.x_scale = y_scale;
    segment.glyphs.y_scale = y_scale;
}

LineBoxLayoutRequest request_for(
    const LineFragmentLayout& fragments,
    const MultiRunShapedText& shaped,
    const FontLineMetricTable& metrics,
    FontFaceId strut_face = 1U,
    std::int32_t strut_scale = 1000) {
    return LineBoxLayoutRequest{
        &fragments,
        &shaped,
        &metrics,
        strut_face,
        strut_scale};
}

void test_empty_line_uses_strut() {
    FontLineMetricTable metrics;
    metrics.records.push_back(metric(1U, 1000U, 800, -200, 0));
    MultiRunShapedText shaped;
    LineFragmentLayout fragments;
    fragments.lines.push_back(VisualLineLayoutRecord{0U, 0U, 0U, 0U, 0U});

    LineBoxLayout output;
    LineBoxLayoutStats stats;
    LineBoxLayoutError error;
    REQUIRE(build_line_box_layout(
        request_for(fragments, shaped, metrics),
        &output,
        &stats,
        &error));
    REQUIRE(output.lines.size() == 1U);
    REQUIRE(output.fragment_metrics.empty());
    REQUIRE(output.lines[0].block_start == 0U);
    REQUIRE(output.lines[0].block_size == 1000U);
    REQUIRE(output.lines[0].baseline == 800U);
    REQUIRE(stats.empty_lines == 1U);
    REQUIRE(stats.total_block_extent == 1000U);
}

void test_matching_fragment_uses_zero_block_offset() {
    FontLineMetricTable metrics;
    metrics.records.push_back(metric(1U, 1000U, 800, -200, 0));
    MultiRunShapedText shaped;
    add_segment(&shaped, 0U, 1U, 1U, 1000);
    LineFragmentLayout fragments;
    fragments.lines.push_back(VisualLineLayoutRecord{500U, 0U, 1U, 1U, 0U});
    fragments.fragments.push_back(InlineLayoutFragment{
        0U, 500U, 0U, 0U, 1U, 0U, 0U, 0U});

    LineBoxLayout output;
    LineBoxLayoutStats stats;
    LineBoxLayoutError error;
    REQUIRE(build_line_box_layout(
        request_for(fragments, shaped, metrics),
        &output,
        &stats,
        &error));
    REQUIRE(output.lines[0].block_size == 1000U);
    REQUIRE(output.lines[0].baseline == 800U);
    REQUIRE(output.fragment_metrics[0].block_offset == 0U);
    REQUIRE(output.fragment_metrics[0].block_size == 1000U);
    REQUIRE(output.fragment_metrics[0].baseline_offset == 800U);
    REQUIRE((output.fragment_metrics[0].flags &
             kFragmentBlockMetricMatchesStrut) != 0U);
}

void test_taller_fallback_expands_line() {
    FontLineMetricTable metrics;
    metrics.records.push_back(metric(1U, 1000U, 800, -200, 0));
    metrics.records.push_back(metric(2U, 1000U, 900, -300, 100));
    MultiRunShapedText shaped;
    add_segment(&shaped, 0U, 1U, 2U, 1000);
    LineFragmentLayout fragments;
    fragments.lines.push_back(VisualLineLayoutRecord{600U, 0U, 1U, 1U, 0U});
    fragments.fragments.push_back(InlineLayoutFragment{
        0U, 600U, 0U, 0U, 1U, 0U, 0U, 0U});

    LineBoxLayout output;
    LineBoxLayoutStats stats;
    LineBoxLayoutError error;
    REQUIRE(build_line_box_layout(
        request_for(fragments, shaped, metrics),
        &output,
        &stats,
        &error));
    REQUIRE(output.lines[0].block_size == 1300U);
    REQUIRE(output.lines[0].baseline == 950U);
    REQUIRE(output.fragment_metrics[0].baseline_offset == 950U);
    REQUIRE(output.fragment_metrics[0].block_size == 1300U);
    REQUIRE((output.lines[0].flags & kLineBoxExpandedBeyondStrut) != 0U);
    REQUIRE((output.lines[0].flags & kLineBoxContainsMixedMetrics) != 0U);
    REQUIRE(stats.expanded_lines == 1U);
    REQUIRE(stats.mixed_metric_lines == 1U);
}

void test_block_positions_accumulate() {
    FontLineMetricTable metrics;
    metrics.records.push_back(metric(1U, 1000U, 800, -200, 0));
    metrics.records.push_back(metric(2U, 1000U, 900, -300, 100));
    MultiRunShapedText shaped;
    add_segment(&shaped, 0U, 1U, 1U, 1000);
    add_segment(&shaped, 1U, 2U, 2U, 1000);
    LineFragmentLayout fragments;
    fragments.lines.push_back(VisualLineLayoutRecord{400U, 0U, 1U, 1U, 0U});
    fragments.lines.push_back(VisualLineLayoutRecord{400U, 1U, 1U, 2U, 0U});
    fragments.fragments.push_back(InlineLayoutFragment{
        0U, 400U, 0U, 0U, 1U, 0U, 0U, 0U});
    fragments.fragments.push_back(InlineLayoutFragment{
        0U, 400U, 1U, 1U, 2U, 0U, 0U, 0U});

    LineBoxLayout output;
    LineBoxLayoutStats stats;
    LineBoxLayoutError error;
    REQUIRE(build_line_box_layout(
        request_for(fragments, shaped, metrics),
        &output,
        &stats,
        &error));
    REQUIRE(output.lines[0].block_start == 0U);
    REQUIRE(output.lines[0].baseline == 800U);
    REQUIRE(output.lines[0].block_size == 1000U);
    REQUIRE(output.lines[1].block_start == 1000U);
    REQUIRE(output.lines[1].baseline == 1950U);
    REQUIRE(output.lines[1].block_size == 1300U);
    REQUIRE(stats.total_block_extent == 2300U);
}

void test_small_fragment_is_centered_on_strut_baseline() {
    FontLineMetricTable metrics;
    metrics.records.push_back(metric(1U, 1000U, 800, -200, 0));
    MultiRunShapedText shaped;
    add_segment(&shaped, 0U, 1U, 1U, 500);
    LineFragmentLayout fragments;
    fragments.lines.push_back(VisualLineLayoutRecord{200U, 0U, 1U, 1U, 0U});
    fragments.fragments.push_back(InlineLayoutFragment{
        0U, 200U, 0U, 0U, 1U, 0U, 0U, 0U});

    LineBoxLayout output;
    LineBoxLayoutStats stats;
    LineBoxLayoutError error;
    REQUIRE(build_line_box_layout(
        request_for(fragments, shaped, metrics),
        &output,
        &stats,
        &error));
    REQUIRE(output.lines[0].block_size == 1000U);
    REQUIRE(output.fragment_metrics[0].block_size == 500U);
    REQUIRE(output.fragment_metrics[0].baseline_offset == 400U);
    REQUIRE(output.fragment_metrics[0].block_offset == 400U);
}

void test_negative_gap_is_supported() {
    FontLineMetricTable metrics;
    metrics.records.push_back(metric(1U, 1000U, 800, -200, -100));
    MultiRunShapedText shaped;
    add_segment(&shaped, 0U, 1U, 1U, 1000);
    LineFragmentLayout fragments;
    fragments.lines.push_back(VisualLineLayoutRecord{200U, 0U, 1U, 1U, 0U});
    fragments.fragments.push_back(InlineLayoutFragment{
        0U, 200U, 0U, 0U, 1U, 0U, 0U, 0U});

    LineBoxLayout output;
    LineBoxLayoutStats stats;
    LineBoxLayoutError error;
    REQUIRE(build_line_box_layout(
        request_for(fragments, shaped, metrics),
        &output,
        &stats,
        &error));
    REQUIRE(output.lines[0].block_size == 900U);
    REQUIRE(output.lines[0].baseline == 750U);
    REQUIRE((output.lines[0].flags & kLineBoxContainsNegativeLineGap) != 0U);
    REQUIRE((output.fragment_metrics[0].flags &
             kFragmentBlockMetricNegativeLineGap) != 0U);
}

void test_missing_face_fails_atomically() {
    FontLineMetricTable metrics;
    metrics.records.push_back(metric(1U, 1000U, 800, -200, 0));
    MultiRunShapedText shaped;
    add_segment(&shaped, 0U, 1U, 2U, 1000);
    LineFragmentLayout fragments;
    fragments.lines.push_back(VisualLineLayoutRecord{100U, 0U, 1U, 1U, 0U});
    fragments.fragments.push_back(InlineLayoutFragment{
        0U, 100U, 0U, 0U, 1U, 0U, 0U, 0U});

    LineBoxLayout output;
    output.lines.push_back(LineBoxRecord{});
    LineBoxLayoutStats stats;
    LineBoxLayoutError error;
    REQUIRE(!build_line_box_layout(
        request_for(fragments, shaped, metrics),
        &output,
        &stats,
        &error));
    REQUIRE(error.kind == LineBoxLayoutErrorKind::MissingFaceMetrics);
    REQUIRE(output.lines.empty());
    REQUIRE(output.fragment_metrics.empty());
}

void test_invalid_partition_fails() {
    FontLineMetricTable metrics;
    metrics.records.push_back(metric(1U, 1000U, 800, -200, 0));
    MultiRunShapedText shaped;
    LineFragmentLayout fragments;
    fragments.lines.push_back(VisualLineLayoutRecord{0U, 1U, 0U, 0U, 0U});

    LineBoxLayout output;
    LineBoxLayoutStats stats;
    LineBoxLayoutError error;
    REQUIRE(!build_line_box_layout(
        request_for(fragments, shaped, metrics),
        &output,
        &stats,
        &error));
    REQUIRE(error.kind == LineBoxLayoutErrorKind::TopologyViolation);
}

class LimitResource final : public std::pmr::memory_resource {
public:
    explicit LimitResource(std::size_t limit) : limit_(limit) {}

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (bytes > limit_ - current_) {
            throw std::bad_alloc();
        }
        void* pointer = upstream_->allocate(bytes, alignment);
        current_ += bytes;
        return pointer;
    }

    void do_deallocate(
        void* pointer,
        std::size_t bytes,
        std::size_t alignment) override {
        upstream_->deallocate(pointer, bytes, alignment);
        current_ -= bytes;
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_{std::pmr::new_delete_resource()};
    std::size_t limit_{0U};
    std::size_t current_{0U};
};

void test_budget_failure_is_atomic() {
    FontLineMetricTable metrics;
    metrics.records.push_back(metric(1U, 1000U, 800, -200, 0));
    MultiRunShapedText shaped;
    add_segment(&shaped, 0U, 1U, 1U, 1000);
    LineFragmentLayout fragments;
    fragments.lines.push_back(VisualLineLayoutRecord{100U, 0U, 1U, 1U, 0U});
    fragments.fragments.push_back(InlineLayoutFragment{
        0U, 100U, 0U, 0U, 1U, 0U, 0U, 0U});

    LimitResource resource(1U);
    LineBoxLayout output(&resource);
    LineBoxLayoutStats stats;
    LineBoxLayoutError error;
    REQUIRE(!build_line_box_layout(
        request_for(fragments, shaped, metrics),
        &output,
        &stats,
        &error));
    REQUIRE(error.kind == LineBoxLayoutErrorKind::OutputBudgetExceeded);
    REQUIRE(output.lines.empty());
    REQUIRE(output.fragment_metrics.empty());
}

} // namespace

int main() {
    test_empty_line_uses_strut();
    test_matching_fragment_uses_zero_block_offset();
    test_taller_fallback_expands_line();
    test_block_positions_accumulate();
    test_small_fragment_is_centered_on_strut_baseline();
    test_negative_gap_is_supported();
    test_missing_face_fails_atomically();
    test_invalid_partition_fails();
    test_budget_failure_is_atomic();
    std::cout << "line box layout tests passed\n";
    return 0;
}
