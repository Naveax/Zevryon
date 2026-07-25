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

void active_clusters(Fixture* fixture, const std::vector<std::uint8_t>& levels) {
    fixture->levels = levels;
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

void segment(
    Fixture* fixture,
    std::uint32_t first,
    std::uint32_t limit,
    std::uint8_t level,
    const std::vector<std::int32_t>& advances) {
    fixture->shaped.segments.emplace_back(fixture->shaped.glyph_resource());
    MultiRunShapedSegment& value = fixture->shaped.segments.back();
    value.run = ShapingRunBoundary{
        first,
        static_cast<FontFaceId>(1U),
        ScriptId::Latn,
        (level & 1U) == 0U
            ? ShapingDirection::LeftToRight
            : ShapingDirection::RightToLeft,
        FontFallbackSource::Primary,
        level,
        0U};
    value.glyphs.first_cluster = first;
    value.glyphs.cluster_limit = limit;
    value.glyphs.script = value.run.script;
    value.glyphs.direction = value.run.direction;
    for (std::size_t index = 0U; index < advances.size(); ++index) {
        value.glyphs.glyphs.push_back(ShapedGlyph{
            static_cast<std::uint32_t>(100U + index),
            static_cast<std::uint32_t>(first + index),
            advances[index],
            0,
            0,
            0,
            0U});
    }
}

LineFragmentLayoutRequest request(
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
        request(fixture, cluster_count, paragraph_level),
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

void singleton_map(Fixture* fixture, std::uint32_t count) {
    for (std::uint32_t cluster = 0U; cluster < count; ++cluster) {
        fixture->cluster_map.records.push_back(GlyphClusterRecord{
            0U, cluster, cluster, 1U});
    }
}

void test_single_ltr_fragment() {
    Fixture fixture;
    active_clusters(&fixture, {0U, 0U, 0U, 0U});
    segment(&fixture, 0U, 4U, 0U, {10, 10, 10, 10});
    singleton_map(&fixture, 4U);
    fixture.selection.lines.push_back(SelectedLineRecord{
        40U, 4U, kSelectedLineMandatoryBreak | kSelectedLineTextEnd});

    LineFragmentLayoutStats stats;
    LineFragmentLayout output = build(fixture, 4U, &stats);
    REQUIRE(output.lines.size() == 1U);
    REQUIRE(output.fragments.size() == 1U);
    REQUIRE(output.fragments[0] == (InlineLayoutFragment{
        0U, 40U, 0U, 0U, 4U, 0U, 0U, 0U}));
    REQUIRE(stats.output_fragments == 1U);
    REQUIRE(stats.total_inline_advance == 40U);
}

void test_nested_levels_reorder_fragments() {
    Fixture fixture;
    const std::vector<std::uint8_t> levels{0U, 1U, 2U, 1U, 0U};
    active_clusters(&fixture, levels);
    for (std::uint32_t cluster = 0U; cluster < 5U; ++cluster) {
        segment(&fixture, cluster, cluster + 1U, levels[cluster], {10});
        fixture.cluster_map.records.push_back(GlyphClusterRecord{
            cluster, cluster, 0U, 1U});
    }
    fixture.selection.lines.push_back(SelectedLineRecord{
        50U, 5U, kSelectedLineMandatoryBreak | kSelectedLineTextEnd});

    LineFragmentLayoutStats stats;
    LineFragmentLayout output = build(fixture, 5U, &stats);
    const std::uint32_t expected[]{0U, 3U, 2U, 1U, 4U};
    REQUIRE(output.fragments.size() == 5U);
    for (std::size_t index = 0U; index < 5U; ++index) {
        REQUIRE(output.fragments[index].segment_index == expected[index]);
        REQUIRE(output.fragments[index].inline_offset == index * 10U);
    }
    REQUIRE((output.lines[0].flags & kVisualLineContainsRtl) != 0U);
    REQUIRE(stats.l2_reversal_spans == 1U);
    REQUIRE(stats.l2_reversed_fragments == 3U);
}

void test_l1_trailing_whitespace_splits_segment() {
    Fixture fixture;
    active_clusters(&fixture, {1U, 1U});
    fixture.units[1].original_class = BidiClass::WS;
    segment(&fixture, 0U, 2U, 1U, {10, 10});
    singleton_map(&fixture, 2U);
    fixture.selection.lines.push_back(SelectedLineRecord{
        20U, 2U, kSelectedLineMandatoryBreak | kSelectedLineTextEnd});

    LineFragmentLayoutStats stats;
    LineFragmentLayout output = build(fixture, 2U, &stats, 0U);
    REQUIRE(output.fragments.size() == 2U);
    REQUIRE(output.fragments[0].bidi_level == 1U);
    REQUIRE(output.fragments[1].bidi_level == 0U);
    REQUIRE((output.fragments[1].flags & kInlineFragmentL1Adjusted) != 0U);
    REQUIRE(stats.bidi_visual.l1_whitespace_resets == 1U);
}

void test_mixed_sign_group_uses_net_advance() {
    Fixture fixture;
    active_clusters(&fixture, {0U});
    segment(&fixture, 0U, 1U, 0U, {10, -3});
    fixture.shaped.segments[0].glyphs.glyphs[1].cluster_index = 0U;
    fixture.cluster_map.records.push_back(GlyphClusterRecord{0U, 0U, 0U, 2U});
    fixture.selection.lines.push_back(SelectedLineRecord{
        7U, 1U, kSelectedLineMandatoryBreak | kSelectedLineTextEnd});
    REQUIRE(build(fixture, 1U).fragments[0].inline_advance == 7U);
}

void test_x9_only_cluster_and_empty_document() {
    Fixture fixture;
    fixture.boundaries = {{0U, 0U}, {1U, 1U}};
    fixture.units.push_back(BidiExplicitUnit{
        0U, 0U, BidiClass::BN, BidiClass::BN, 0U, kBidiUnitRemovedByX9});
    segment(&fixture, 0U, 1U, 0U, {0});
    fixture.cluster_map.records.push_back(GlyphClusterRecord{0U, 0U, 0U, 1U});
    fixture.selection.lines.push_back(SelectedLineRecord{
        0U, 1U, kSelectedLineMandatoryBreak | kSelectedLineTextEnd});

    LineFragmentLayoutStats stats;
    LineFragmentLayout output = build(fixture, 1U, &stats);
    REQUIRE((output.fragments[0].flags & kInlineFragmentContainsX9Only) != 0U);
    REQUIRE(stats.zero_active_lines == 1U);

    Fixture empty;
    empty.selection.lines.push_back(SelectedLineRecord{
        0U,
        0U,
        kSelectedLineMandatoryBreak | kSelectedLineTextEnd | kSelectedLineEmpty});
    LineFragmentLayout empty_output = build(empty, 0U);
    REQUIRE(empty_output.lines.size() == 1U);
    REQUIRE(empty_output.fragments.empty());
}

void test_invalid_boundaries_and_advances_fail_closed() {
    Fixture merged;
    active_clusters(&merged, {0U, 0U});
    segment(&merged, 0U, 2U, 0U, {20});
    merged.cluster_map.records.push_back(GlyphClusterRecord{0U, 0U, 0U, 1U});
    merged.cluster_map.records.push_back(GlyphClusterRecord{0U, 0U, 0U, 1U});
    merged.selection.lines.push_back(SelectedLineRecord{20U, 1U, kSelectedLineSoftBreak});
    merged.selection.lines.push_back(SelectedLineRecord{
        0U, 2U, kSelectedLineMandatoryBreak | kSelectedLineTextEnd});

    LineFragmentLayout output;
    LineFragmentLayoutStats stats;
    LineFragmentLayoutError error;
    REQUIRE(!build_line_fragment_layout(request(merged, 2U), &output, &stats, &error));
    REQUIRE(error.kind == LineFragmentLayoutErrorKind::UnsafeFragmentBoundary);
    REQUIRE(output.lines.empty() && output.fragments.empty());

    Fixture mismatch;
    active_clusters(&mismatch, {0U});
    segment(&mismatch, 0U, 1U, 0U, {10});
    mismatch.cluster_map.records.push_back(GlyphClusterRecord{0U, 0U, 0U, 1U});
    mismatch.selection.lines.push_back(SelectedLineRecord{
        9U, 1U, kSelectedLineMandatoryBreak | kSelectedLineTextEnd});
    REQUIRE(!build_line_fragment_layout(request(mismatch, 1U), &output, &stats, &error));
    REQUIRE(error.kind == LineFragmentLayoutErrorKind::TopologyViolation);
    REQUIRE(output.lines.empty() && output.fragments.empty());
}

class LimitResource final : public std::pmr::memory_resource {
public:
    explicit LimitResource(std::size_t limit) : limit_(limit) {}

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (current_ > limit_ || bytes > limit_ - current_) {
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
    Fixture fixture;
    active_clusters(&fixture, {0U});
    segment(&fixture, 0U, 1U, 0U, {10});
    fixture.cluster_map.records.push_back(GlyphClusterRecord{0U, 0U, 0U, 1U});
    fixture.selection.lines.push_back(SelectedLineRecord{
        10U, 1U, kSelectedLineMandatoryBreak | kSelectedLineTextEnd});

    LimitResource resource(1U);
    LineFragmentLayout output(&resource);
    LineFragmentLayoutStats stats;
    LineFragmentLayoutError error;
    REQUIRE(!build_line_fragment_layout(request(fixture, 1U), &output, &stats, &error));
    REQUIRE(error.kind == LineFragmentLayoutErrorKind::OutputBudgetExceeded);
    REQUIRE(output.lines.empty() && output.fragments.empty());
}

} // namespace

int main() {
    test_single_ltr_fragment();
    test_nested_levels_reorder_fragments();
    test_l1_trailing_whitespace_splits_segment();
    test_mixed_sign_group_uses_net_advance();
    test_x9_only_cluster_and_empty_document();
    test_invalid_boundaries_and_advances_fail_closed();
    test_budget_failure_is_atomic();
    std::cout << "line fragment layout tests passed\n";
    return 0;
}
