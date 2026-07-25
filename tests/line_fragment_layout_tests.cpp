#include "line_fragment_layout.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory_resource>
#include <new>
#include <vector>

namespace {

using namespace zevryon::text;

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        std::cerr << "requirement failed: " #condition \
                  << " at " << __FILE__ << ':' << __LINE__ << '\n'; \
        std::abort(); \
    } \
} while (false)

struct Fixture final {
    std::vector<GraphemeBoundary> boundaries;
    std::vector<BidiExplicitUnit> units;
    BidiSequenceTopology topology;
    std::vector<std::uint8_t> levels;
    MultiRunShapedText shaped;
    GlyphClusterMap cluster_map;
    LineSelection selection;
};

void build_active_clusters(
    Fixture* fixture,
    const std::vector<std::uint8_t>& levels) {
    fixture->boundaries.clear();
    fixture->units.clear();
    fixture->levels = levels;
    fixture->topology.active_unit_indices.clear();
    for (std::size_t index = 0U; index <= levels.size(); ++index) {
        fixture->boundaries.push_back(GraphemeBoundary{
            static_cast<std::uint64_t>(index),
            static_cast<std::uint32_t>(index)});
    }
    for (std::size_t index = 0U; index < levels.size(); ++index) {
        fixture->units.push_back(BidiExplicitUnit{
            static_cast<std::uint64_t>(index),
            static_cast<std::uint32_t>(index),
            BidiClass::L,
            BidiClass::L,
            levels[index],
            0U});
        fixture->topology.active_unit_indices.push_back(
            static_cast<std::uint32_t>(index));
    }
}

void add_segment(
    Fixture* fixture,
    std::uint32_t first_cluster,
    std::uint32_t cluster_limit,
    std::uint8_t level,
    const std::vector<std::int32_t>& advances) {
    fixture->shaped.segments.emplace_back(
        fixture->shaped.glyph_resource());
    MultiRunShapedSegment& segment = fixture->shaped.segments.back();
    segment.run = ShapingRunBoundary{
        first_cluster,
        static_cast<FontFaceId>(1U),
        ScriptId::Latn,
        (level & 1U) == 0U
            ? ShapingDirection::LeftToRight
            : ShapingDirection::RightToLeft,
        FontFallbackSource::Primary,
        level,
        0U};
    segment.glyphs.first_cluster = first_cluster;
    segment.glyphs.cluster_limit = cluster_limit;
    segment.glyphs.script = segment.run.script;
    segment.glyphs.direction = segment.run.direction;
    for (std::size_t index = 0U; index < advances.size(); ++index) {
        segment.glyphs.glyphs.push_back(ShapedGlyph{
            static_cast<std::uint32_t>(100U + index),
            static_cast<std::uint32_t>(first_cluster + index),
            advances[index],
            0,
            0,
            0,
            0U});
    }
}

LineFragmentLayoutRequest request_for(
    const Fixture& fixture,
    std::uint32_t cluster_count,
    std::uint8_t paragraph_level = 0U) {
    return LineFragmentLayoutRequest{
        fixture.boundaries,
        fixture.units,
        &fixture.topology,
        fixture.levels,
        paragraph_level,
        &fixture.shaped,
        &fixture.cluster_map,
        &fixture.selection,
        cluster_count};
}

LineFragmentLayout build(
    const Fixture& fixture,
    std::uint32_t cluster_count,
    LineFragmentLayoutStats* stats = nullptr,
    std::uint8_t paragraph_level = 0U) {
    LineFragmentLayout output;
    LineFragmentLayoutStats local_stats;
    LineFragmentLayoutError error;
    const bool ok = build_line_fragment_layout(
        request_for(fixture, cluster_count, paragraph_level),
        &output,
        stats != nullptr ? stats : &local_stats,
        &error);
    if (!ok) {
        std::cerr << line_fragment_layout_error_kind_name(error.kind)
                  << ": " << error.message << '\n';
    }
    REQUIRE(ok);
    return output;
}

void test_single_ltr_fragment() {
    Fixture fixture;
    build_active_clusters(&fixture, {0U, 0U, 0U, 0U});
    add_segment(&fixture, 0U, 4U, 0U, {10, 10, 10, 10});
    for (std::uint32_t cluster = 0U; cluster < 4U; ++cluster) {
        fixture.cluster_map.records.push_back(GlyphClusterRecord{
            0U, cluster, cluster, 1U});
    }
    fixture.selection.lines.push_back(SelectedLineRecord{
        40U,
        4U,
        kSelectedLineMandatoryBreak | kSelectedLineTextEnd});

    LineFragmentLayoutStats stats;
    LineFragmentLayout output = build(fixture, 4U, &stats);
    REQUIRE(output.lines.size() == 1U);
    REQUIRE(output.fragments.size() == 1U);
    REQUIRE(output.lines[0] == (VisualLineLayoutRecord{
        40U,
        0U,
        1U,
        4U,
        kSelectedLineMandatoryBreak | kSelectedLineTextEnd}));
    REQUIRE(output.fragments[0] == (InlineLayoutFragment{
        0U, 40U, 0U, 0U, 4U, 0U, 0U, 0U}));
    REQUIRE(stats.output_lines == 1U);
    REQUIRE(stats.output_fragments == 1U);
    REQUIRE(stats.total_inline_advance == 40U);
}

void test_nested_levels_reorder_fragment_runs() {
    Fixture fixture;
    const std::vector<std::uint8_t> levels{0U, 1U, 2U, 1U, 0U};
    build_active_clusters(&fixture, levels);
    for (std::uint32_t cluster = 0U; cluster < 5U; ++cluster) {
        add_segment(
            &fixture,
            cluster,
            cluster + 1U,
            levels[cluster],
            {10});
        fixture.cluster_map.records.push_back(GlyphClusterRecord{
            cluster, cluster, 0U, 1U});
    }
    fixture.selection.lines.push_back(SelectedLineRecord{
        50U,
        5U,
        kSelectedLineMandatoryBreak | kSelectedLineTextEnd});

    LineFragmentLayoutStats stats;
    LineFragmentLayout output = build(fixture, 5U, &stats);
    REQUIRE(output.fragments.size() == 5U);
    const std::uint32_t expected_segments[]{0U, 3U, 2U, 1U, 4U};
    const std::uint8_t expected_levels[]{0U, 1U, 2U, 1U, 0U};
    for (std::size_t index = 0U; index < 5U; ++index) {
        REQUIRE(output.fragments[index].segment_index == expected_segments[index]);
        REQUIRE(output.fragments[index].bidi_level == expected_levels[index]);
        REQUIRE(output.fragments[index].inline_offset == index * 10U);
        REQUIRE(output.fragments[index].inline_advance == 10U);
    }
    REQUIRE((output.lines[0].flags & kVisualLineContainsRtl) != 0U);
    REQUIRE(stats.l2_reversal_spans == 1U);
    REQUIRE(stats.l2_reversed_fragments == 3U);
    REQUIRE(stats.maximum_fragment_level == 2U);
}

void test_l1_trailing_whitespace_splits_a_segment() {
    Fixture fixture;
    build_active_clusters(&fixture, {1U, 1U});
    fixture.units[1].original_class = BidiClass::WS;
    add_segment(&fixture, 0U, 2U, 1U, {10, 10});
    fixture.cluster_map.records.push_back(GlyphClusterRecord{0U, 0U, 0U, 1U});
    fixture.cluster_map.records.push_back(GlyphClusterRecord{0U, 1U, 1U, 1U});
    fixture.selection.lines.push_back(SelectedLineRecord{
        20U,
        2U,
        kSelectedLineMandatoryBreak | kSelectedLineTextEnd});

    LineFragmentLayoutStats stats;
    LineFragmentLayout output = build(fixture, 2U, &stats, 0U);
    REQUIRE(output.fragments.size() == 2U);
    REQUIRE(output.fragments[0].bidi_level == 1U);
    REQUIRE(output.fragments[1].bidi_level == 0U);
    REQUIRE((output.fragments[1].flags & kInlineFragmentL1Adjusted) != 0U);
    REQUIRE((output.lines[0].flags & kVisualLineL1Adjusted) != 0U);
    REQUIRE(stats.l1_adjusted_clusters == 1U);
    REQUIRE(stats.l1_adjusted_fragments == 1U);
    REQUIRE(stats.bidi_visual.l1_whitespace_resets == 1U);
}

void test_mixed_sign_glyph_group_uses_net_advance() {
    Fixture fixture;
    build_active_clusters(&fixture, {0U});
    add_segment(&fixture, 0U, 1U, 0U, {10, -3});
    fixture.shaped.segments[0].glyphs.glyphs[1].cluster_index = 0U;
    fixture.cluster_map.records.push_back(GlyphClusterRecord{0U, 0U, 0U, 2U});
    fixture.selection.lines.push_back(SelectedLineRecord{
        7U,
        1U,
        kSelectedLineMandatoryBreak | kSelectedLineTextEnd});

    LineFragmentLayout output = build(fixture, 1U);
    REQUIRE(output.fragments.size() == 1U);
    REQUIRE(output.fragments[0].inline_advance == 7U);
}

void test_x9_only_cluster_is_retained_without_active_line_span() {
    Fixture fixture;
    fixture.boundaries.push_back(GraphemeBoundary{0U, 0U});
    fixture.boundaries.push_back(GraphemeBoundary{1U, 1U});
    fixture.units.push_back(BidiExplicitUnit{
        0U,
        0U,
        BidiClass::BN,
        BidiClass::BN,
        0U,
        kBidiUnitRemovedByX9});
    add_segment(&fixture, 0U, 1U, 0U, {0});
    fixture.cluster_map.records.push_back(GlyphClusterRecord{0U, 0U, 0U, 1U});
    fixture.selection.lines.push_back(SelectedLineRecord{
        0U,
        1U,
        kSelectedLineMandatoryBreak | kSelectedLineTextEnd});

    LineFragmentLayoutStats stats;
    LineFragmentLayout output = build(fixture, 1U, &stats);
    REQUIRE(output.fragments.size() == 1U);
    REQUIRE((output.fragments[0].flags & kInlineFragmentContainsX9Only) != 0U);
    REQUIRE((output.lines[0].flags & kVisualLineContainsX9Only) != 0U);
    REQUIRE(stats.zero_active_lines == 1U);
    REQUIRE(stats.zero_active_clusters == 1U);
    REQUIRE(stats.x9_only_fragments == 1U);
}

void test_empty_document_has_one_line_and_no_fragments() {
    Fixture fixture;
    fixture.selection.lines.push_back(SelectedLineRecord{
        0U,
        0U,
        kSelectedLineMandatoryBreak |
            kSelectedLineTextEnd |
            kSelectedLineEmpty});

    LineFragmentLayout output = build(fixture, 0U);
    REQUIRE(output.lines.size() == 1U);
    REQUIRE(output.fragments.empty());
    REQUIRE(output.lines[0].fragment_count == 0U);
    REQUIRE(output.lines[0].cluster_limit == 0U);
}

void test_merged_group_line_boundary_is_rejected() {
    Fixture fixture;
    build_active_clusters(&fixture, {0U, 0U});
    add_segment(&fixture, 0U, 2U, 0U, {20});
    fixture.cluster_map.records.push_back(GlyphClusterRecord{0U, 0U, 0U, 1U});
    fixture.cluster_map.records.push_back(GlyphClusterRecord{0U, 0U, 0U, 1U});
    fixture.selection.lines.push_back(SelectedLineRecord{
        20U,
        1U,
        kSelectedLineSoftBreak});
    fixture.selection.lines.push_back(SelectedLineRecord{
        0U,
        2U,
        kSelectedLineMandatoryBreak | kSelectedLineTextEnd});

    LineFragmentLayout output;
    LineFragmentLayoutStats stats;
    LineFragmentLayoutError error;
    REQUIRE(!build_line_fragment_layout(
        request_for(fixture, 2U),
        &output,
        &stats,
        &error));
    REQUIRE(error.kind == LineFragmentLayoutErrorKind::UnsafeFragmentBoundary);
    REQUIRE(output.lines.empty());
    REQUIRE(output.fragments.empty());
}

void test_line_advance_mismatch_fails_closed() {
    Fixture fixture;
    build_active_clusters(&fixture, {0U});
    add_segment(&fixture, 0U, 1U, 0U, {10});
    fixture.cluster_map.records.push_back(GlyphClusterRecord{0U, 0U, 0U, 1U});
    fixture.selection.lines.push_back(SelectedLineRecord{
        9U,
        1U,
        kSelectedLineMandatoryBreak | kSelectedLineTextEnd});

    LineFragmentLayout output;
    LineFragmentLayoutStats stats;
    LineFragmentLayoutError error;
    REQUIRE(!build_line_fragment_layout(
        request_for(fixture, 1U),
        &output,
        &stats,
        &error));
    REQUIRE(error.kind == LineFragmentLayoutErrorKind::TopologyViolation);
    REQUIRE(output.lines.empty());
    REQUIRE(output.fragments.empty());
}

class LimitResource final : public std::pmr::memory_resource {
public:
    explicit LimitResource(std::size_t limit) : limit_(limit) {}

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (bytes > limit_ - current_) {
            throw std::bad_alloc();
        }
        void* pointer = upstream_.allocate(bytes, alignment);
        current_ += bytes;
        return pointer;
    }

    void do_deallocate(
        void* pointer,
        std::size_t bytes,
        std::size_t alignment) override {
        upstream_.deallocate(pointer, bytes, alignment);
        current_ -= bytes;
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::new_delete_resource_t* unused_{nullptr};
    std::pmr::memory_resource& upstream_{*std::pmr::new_delete_resource()};
    std::size_t limit_{0U};
    std::size_t current_{0U};
};

void test_budget_failure_is_atomic() {
    Fixture fixture;
    build_active_clusters(&fixture, {0U});
    add_segment(&fixture, 0U, 1U, 0U, {10});
    fixture.cluster_map.records.push_back(GlyphClusterRecord{0U, 0U, 0U, 1U});
    fixture.selection.lines.push_back(SelectedLineRecord{
        10U,
        1U,
        kSelectedLineMandatoryBreak | kSelectedLineTextEnd});

    LimitResource resource(1U);
    LineFragmentLayout output(&resource);
    LineFragmentLayoutStats stats;
    LineFragmentLayoutError error;
    REQUIRE(!build_line_fragment_layout(
        request_for(fixture, 1U),
        &output,
        &stats,
        &error));
    REQUIRE(error.kind == LineFragmentLayoutErrorKind::OutputBudgetExceeded);
    REQUIRE(output.lines.empty());
    REQUIRE(output.fragments.empty());
}

} // namespace

int main() {
    test_single_ltr_fragment();
    test_nested_levels_reorder_fragment_runs();
    test_l1_trailing_whitespace_splits_a_segment();
    test_mixed_sign_glyph_group_uses_net_advance();
    test_x9_only_cluster_is_retained_without_active_line_span();
    test_empty_document_has_one_line_and_no_fragments();
    test_merged_group_line_boundary_is_rejected();
    test_line_advance_mismatch_fails_closed();
    test_budget_failure_is_atomic();
    std::cout << "line fragment layout tests passed\n";
    return 0;
}
