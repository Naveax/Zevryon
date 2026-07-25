#include "line_selection.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory_resource>
#include <new>
#include <span>
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
    MultiRunShapedText shaped_text;
    GlyphClusterMap cluster_map;
    CaretBoundaryMap caret_map;
    LineBreakOpportunityMap opportunity_map;
    std::uint32_t cluster_count{0};
};

void append_opportunities(
    Fixture* fixture,
    std::initializer_list<LineBreakOpportunity> values) {
    for (LineBreakOpportunity value : values) {
        fixture->opportunity_map.opportunities.push_back(
            static_cast<std::uint8_t>(value));
    }
}

void append_caret_safety(
    Fixture* fixture,
    std::initializer_list<bool> values) {
    for (bool value : values) {
        fixture->caret_map.flags.push_back(
            value
                ? static_cast<std::uint8_t>(kCaretBoundarySafe)
                : static_cast<std::uint8_t>(0U));
    }
}

void build_singleton_fixture(
    Fixture* fixture,
    std::initializer_list<std::int32_t> advances,
    ShapingDirection direction = ShapingDirection::LeftToRight) {
    fixture->cluster_count = static_cast<std::uint32_t>(advances.size());
    if (fixture->cluster_count == 0U) {
        return;
    }

    fixture->shaped_text.segments.emplace_back(
        fixture->shaped_text.glyph_resource());
    MultiRunShapedSegment& segment = fixture->shaped_text.segments.back();
    segment.run = ShapingRunBoundary{
        0U,
        static_cast<FontFaceId>(1U),
        ScriptId::Latn,
        direction,
        FontFallbackSource::Primary,
        static_cast<std::uint8_t>(
            direction == ShapingDirection::RightToLeft ? 1U : 0U),
        0U};
    segment.glyphs.first_cluster = 0U;
    segment.glyphs.cluster_limit = fixture->cluster_count;
    segment.glyphs.script = segment.run.script;
    segment.glyphs.direction = segment.run.direction;

    std::uint32_t cluster_index = 0U;
    for (std::int32_t advance : advances) {
        segment.glyphs.glyphs.push_back(ShapedGlyph{
            100U + cluster_index,
            cluster_index,
            advance,
            0,
            0,
            0,
            0U});
        fixture->cluster_map.records.push_back(GlyphClusterRecord{
            0U,
            cluster_index,
            cluster_index,
            1U});
        ++cluster_index;
    }
}

LineSelection select(
    Fixture* fixture,
    std::uint64_t available_inline_advance,
    LineSelectionStats* stats = nullptr) {
    LineSelection output;
    LineSelectionStats local_stats;
    LineSelectionError error;
    const bool ok = select_bounded_lines(
        LineSelectionRequest{
            &fixture->shaped_text,
            &fixture->cluster_map,
            &fixture->caret_map,
            &fixture->opportunity_map,
            fixture->cluster_count,
            available_inline_advance},
        &output,
        stats != nullptr ? stats : &local_stats,
        &error);
    if (!ok) {
        std::cerr << line_selection_error_kind_name(error.kind)
                  << ": " << error.message << '\n';
    }
    REQUIRE(ok);
    return output;
}

void test_last_fitting_boundary_is_selected() {
    Fixture fixture;
    build_singleton_fixture(&fixture, {4, 4, 4, 4});
    append_opportunities(
        &fixture,
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Allowed,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Mandatory
        });
    append_caret_safety(&fixture, {true, true, true, true, true});

    LineSelectionStats stats;
    LineSelection output = select(&fixture, 10U, &stats);
    REQUIRE(output.lines.size() == 2U);
    REQUIRE(output.lines[0] == SelectedLineRecord{
        8U,
        2U,
        kSelectedLineSoftBreak});
    REQUIRE(output.lines[1] == SelectedLineRecord{
        8U,
        4U,
        kSelectedLineMandatoryBreak | kSelectedLineTextEnd});
    REQUIRE(selected_line_first_cluster(output, 0U) == 0U);
    REQUIRE(selected_line_first_cluster(output, 1U) == 2U);
    REQUIRE(stats.output_lines == 2U);
    REQUIRE(stats.soft_break_lines == 1U);
    REQUIRE(stats.mandatory_break_lines == 1U);
    REQUIRE(stats.overflow_lines == 0U);
    REQUIRE(stats.total_inline_advance == 16U);
}

void test_unbreakable_span_overflows_at_next_legal_boundary() {
    Fixture fixture;
    build_singleton_fixture(&fixture, {6, 6});
    append_opportunities(
        &fixture,
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Mandatory
        });
    append_caret_safety(&fixture, {true, true, true});

    LineSelection output = select(&fixture, 10U);
    REQUIRE(output.lines.size() == 1U);
    REQUIRE(output.lines[0].inline_advance == 12U);
    REQUIRE(output.lines[0].cluster_limit == 2U);
    REQUIRE(selected_line_has_flag(
        output,
        0U,
        kSelectedLineOverflow));
    REQUIRE(selected_line_has_flag(
        output,
        0U,
        kSelectedLineMandatoryBreak));
}

void test_allowed_boundary_can_close_an_overflow_line() {
    Fixture fixture;
    build_singleton_fixture(&fixture, {12, 4});
    append_opportunities(
        &fixture,
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Allowed,
            LineBreakOpportunity::Mandatory
        });
    append_caret_safety(&fixture, {true, true, true});

    LineSelection output = select(&fixture, 10U);
    REQUIRE(output.lines.size() == 2U);
    REQUIRE(output.lines[0].inline_advance == 12U);
    REQUIRE(output.lines[0].cluster_limit == 1U);
    REQUIRE(selected_line_has_flag(
        output,
        0U,
        kSelectedLineSoftBreak));
    REQUIRE(selected_line_has_flag(
        output,
        0U,
        kSelectedLineOverflow));
    REQUIRE(output.lines[1].inline_advance == 4U);
}

void test_soft_wrap_precedes_mandatory_boundary_when_needed() {
    Fixture fixture;
    build_singleton_fixture(&fixture, {4, 4, 4});
    append_opportunities(
        &fixture,
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Allowed,
            LineBreakOpportunity::Mandatory
        });
    append_caret_safety(&fixture, {true, true, true, true});

    LineSelection output = select(&fixture, 8U);
    REQUIRE(output.lines.size() == 2U);
    REQUIRE(output.lines[0].cluster_limit == 2U);
    REQUIRE(output.lines[0].inline_advance == 8U);
    REQUIRE(output.lines[1].cluster_limit == 3U);
    REQUIRE(output.lines[1].inline_advance == 4U);
    REQUIRE(selected_line_has_flag(
        output,
        1U,
        kSelectedLineMandatoryBreak));
}

void test_unsafe_allowed_boundary_is_suppressed() {
    Fixture fixture;
    build_singleton_fixture(&fixture, {6, 6});
    append_opportunities(
        &fixture,
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Allowed,
            LineBreakOpportunity::Mandatory
        });
    append_caret_safety(&fixture, {true, false, true});

    LineSelectionStats stats;
    LineSelection output = select(&fixture, 10U, &stats);
    REQUIRE(output.lines.size() == 1U);
    REQUIRE(selected_line_has_flag(
        output,
        0U,
        kSelectedLineOverflow));
    REQUIRE(stats.suppressed_unsafe_boundaries == 1U);
    REQUIRE(stats.legal_boundaries == 1U);
}

void test_certified_cluster_and_caret_maps_feed_selection() {
    Fixture fixture;
    fixture.cluster_count = 4U;
    fixture.shaped_text.segments.emplace_back(
        fixture.shaped_text.glyph_resource());
    MultiRunShapedSegment& segment = fixture.shaped_text.segments.back();
    segment.run = ShapingRunBoundary{
        0U,
        static_cast<FontFaceId>(2U),
        ScriptId::Latn,
        ShapingDirection::LeftToRight,
        FontFallbackSource::Primary,
        0U,
        0U};
    segment.glyphs.first_cluster = 0U;
    segment.glyphs.cluster_limit = 4U;
    segment.glyphs.script = segment.run.script;
    segment.glyphs.direction = segment.run.direction;
    segment.glyphs.glyphs.push_back(ShapedGlyph{
        201U, 0U, 64, 0, 0, 0, 0U});
    segment.glyphs.glyphs.push_back(ShapedGlyph{
        202U, 2U, 64, 0, 0, 0, 0U});

    GlyphClusterMapStats cluster_stats;
    GlyphClusterMapError cluster_error;
    REQUIRE(build_glyph_cluster_map(
        fixture.shaped_text,
        fixture.cluster_count,
        &fixture.cluster_map,
        &cluster_stats,
        &cluster_error));

    CaretBoundaryMapStats caret_stats;
    CaretBoundaryMapError caret_error;
    REQUIRE(build_caret_boundary_map(
        CaretBoundaryMapRequest{
            &fixture.shaped_text,
            &fixture.cluster_map,
            fixture.cluster_count},
        &fixture.caret_map,
        &caret_stats,
        &caret_error));

    append_opportunities(
        &fixture,
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Allowed,
            LineBreakOpportunity::Allowed,
            LineBreakOpportunity::Allowed,
            LineBreakOpportunity::Mandatory
        });

    LineSelectionStats stats;
    LineSelection output = select(&fixture, 64U, &stats);
    REQUIRE(output.lines.size() == 2U);
    REQUIRE(output.lines[0].cluster_limit == 2U);
    REQUIRE(output.lines[0].inline_advance == 64U);
    REQUIRE(output.lines[1].cluster_limit == 4U);
    REQUIRE(output.lines[1].inline_advance == 64U);
    REQUIRE(stats.suppressed_unsafe_boundaries == 2U);
    REQUIRE(stats.zero_advance_clusters == 2U);
}

void test_rtl_advance_uses_magnitude() {
    Fixture fixture;
    build_singleton_fixture(
        &fixture,
        {-6, -6},
        ShapingDirection::RightToLeft);
    append_opportunities(
        &fixture,
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Mandatory
        });
    append_caret_safety(&fixture, {true, true, true});

    LineSelection output = select(&fixture, 20U);
    REQUIRE(output.lines.size() == 1U);
    REQUIRE(output.lines[0].inline_advance == 12U);
    REQUIRE(!selected_line_has_flag(
        output,
        0U,
        kSelectedLineOverflow));
}

void test_zero_width_and_empty_document() {
    Fixture nonempty;
    build_singleton_fixture(&nonempty, {1});
    append_opportunities(
        &nonempty,
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Mandatory
        });
    append_caret_safety(&nonempty, {true, true});
    LineSelection overflow = select(&nonempty, 0U);
    REQUIRE(selected_line_has_flag(
        overflow,
        0U,
        kSelectedLineOverflow));

    Fixture empty;
    append_opportunities(
        &empty,
        {LineBreakOpportunity::Mandatory});
    append_caret_safety(&empty, {true});
    LineSelection output = select(&empty, 0U);
    REQUIRE(output.lines.size() == 1U);
    REQUIRE(output.lines[0].cluster_limit == 0U);
    REQUIRE(output.lines[0].inline_advance == 0U);
    REQUIRE(selected_line_has_flag(
        output,
        0U,
        kSelectedLineEmpty));
    REQUIRE(selected_line_has_flag(
        output,
        0U,
        kSelectedLineTextEnd));
}

class LimitResource final : public std::pmr::memory_resource {
public:
    explicit LimitResource(std::size_t limit) : limit_(limit) {}

    std::size_t current() const noexcept {
        return current_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (bytes > limit_ - current_) {
            throw std::bad_alloc();
        }
        void* result = std::pmr::new_delete_resource()->allocate(
            bytes,
            alignment);
        current_ += bytes;
        return result;
    }

    void do_deallocate(
        void* pointer,
        std::size_t bytes,
        std::size_t alignment) override {
        REQUIRE(current_ >= bytes);
        std::pmr::new_delete_resource()->deallocate(
            pointer,
            bytes,
            alignment);
        current_ -= bytes;
    }

    bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t limit_{0};
    std::size_t current_{0};
};

void test_budget_failure_is_atomic() {
    Fixture fixture;
    build_singleton_fixture(&fixture, {4, 4, 4, 4});
    append_opportunities(
        &fixture,
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Allowed,
            LineBreakOpportunity::Allowed,
            LineBreakOpportunity::Allowed,
            LineBreakOpportunity::Mandatory
        });
    append_caret_safety(&fixture, {true, true, true, true, true});

    LimitResource resource(sizeof(SelectedLineRecord));
    LineSelection output(&resource);
    output.lines.push_back(SelectedLineRecord{
        1U,
        1U,
        kSelectedLineSoftBreak});
    REQUIRE(resource.current() == sizeof(SelectedLineRecord));

    LineSelectionStats stats;
    LineSelectionError error;
    REQUIRE(!select_bounded_lines(
        LineSelectionRequest{
            &fixture.shaped_text,
            &fixture.cluster_map,
            &fixture.caret_map,
            &fixture.opportunity_map,
            fixture.cluster_count,
            8U},
        &output,
        &stats,
        &error));
    REQUIRE(error.kind == LineSelectionErrorKind::OutputBudgetExceeded);
    REQUIRE(output.lines.empty());
    REQUIRE(resource.current() == 0U);
}

void test_invalid_topology_and_vertical_direction_fail_closed() {
    Fixture invalid_terminal;
    build_singleton_fixture(&invalid_terminal, {4});
    append_opportunities(
        &invalid_terminal,
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Allowed
        });
    append_caret_safety(&invalid_terminal, {true, true});

    LineSelection output;
    LineSelectionStats stats;
    LineSelectionError error;
    REQUIRE(!select_bounded_lines(
        LineSelectionRequest{
            &invalid_terminal.shaped_text,
            &invalid_terminal.cluster_map,
            &invalid_terminal.caret_map,
            &invalid_terminal.opportunity_map,
            invalid_terminal.cluster_count,
            8U},
        &output,
        &stats,
        &error));
    REQUIRE(error.kind == LineSelectionErrorKind::InconsistentTopology);
    REQUIRE(output.lines.empty());

    Fixture vertical;
    build_singleton_fixture(
        &vertical,
        {4},
        ShapingDirection::TopToBottom);
    append_opportunities(
        &vertical,
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Mandatory
        });
    append_caret_safety(&vertical, {true, true});
    REQUIRE(!select_bounded_lines(
        LineSelectionRequest{
            &vertical.shaped_text,
            &vertical.cluster_map,
            &vertical.caret_map,
            &vertical.opportunity_map,
            vertical.cluster_count,
            8U},
        &output,
        &stats,
        &error));
    REQUIRE(error.kind == LineSelectionErrorKind::UnsupportedDirection);
    REQUIRE(output.lines.empty());
}

} // namespace

int main() {
    test_last_fitting_boundary_is_selected();
    test_unbreakable_span_overflows_at_next_legal_boundary();
    test_allowed_boundary_can_close_an_overflow_line();
    test_soft_wrap_precedes_mandatory_boundary_when_needed();
    test_unsafe_allowed_boundary_is_suppressed();
    test_certified_cluster_and_caret_maps_feed_selection();
    test_rtl_advance_uses_magnitude();
    test_zero_width_and_empty_document();
    test_budget_failure_is_atomic();
    test_invalid_topology_and_vertical_direction_fail_closed();
    std::cout << "line selection tests passed\n";
    return 0;
}
