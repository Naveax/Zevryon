#include "viewport_projection.hpp"

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
        return false;
    }
    return true;
}

class CappedResource final : public std::pmr::memory_resource {
public:
    explicit CappedResource(std::size_t cap) : cap_(cap) {}

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (bytes > cap_ - used_) {
            throw std::bad_alloc();
        }
        void* value = std::pmr::new_delete_resource()->allocate(
            bytes,
            alignment);
        used_ += bytes;
        return value;
    }

    void do_deallocate(
        void* pointer,
        std::size_t bytes,
        std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(
            pointer,
            bytes,
            alignment);
        used_ -= bytes;
    }

    bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t cap_{0};
    std::size_t used_{0};
};

struct Fixture final {
    MultiRunShapedText shaped;
    GlyphClusterMap clusters;
    CaretBoundaryMap caret;
    LineFragmentLayout fragments;
    LineBoxLayout boxes;

    Fixture()
        : shaped(std::pmr::get_default_resource()),
          clusters(std::pmr::get_default_resource()),
          caret(std::pmr::get_default_resource()),
          fragments(std::pmr::get_default_resource()),
          boxes(std::pmr::get_default_resource()) {
        shaped.segments.emplace_back(std::pmr::get_default_resource());
        shaped.segments.back().glyphs.direction =
            ShapingDirection::LeftToRight;
        shaped.segments.back().glyphs.glyphs.push_back(
            {1U, 0U, 10, 0, 0, 0, 0U});
        shaped.segments.back().glyphs.glyphs.push_back(
            {2U, 1U, 20, 0, 0, 0, 0U});

        shaped.segments.emplace_back(std::pmr::get_default_resource());
        shaped.segments.back().glyphs.direction =
            ShapingDirection::RightToLeft;
        shaped.segments.back().glyphs.glyphs.push_back(
            {3U, 3U, -30, 0, 0, 0, 0U});
        shaped.segments.back().glyphs.glyphs.push_back(
            {4U, 2U, -40, 0, 0, 0, 0U});

        shaped.segments.emplace_back(std::pmr::get_default_resource());
        shaped.segments.back().glyphs.direction =
            ShapingDirection::LeftToRight;
        shaped.segments.back().glyphs.glyphs.push_back(
            {5U, 4U, 12, 0, 0, 0, 0U});
        shaped.segments.back().glyphs.glyphs.push_back(
            {6U, 5U, 8, 0, 0, 0, 0U});

        clusters.records.push_back({0U, 0U, 0U, 1U});
        clusters.records.push_back({0U, 1U, 1U, 1U});
        clusters.records.push_back({1U, 2U, 1U, 1U});
        clusters.records.push_back({1U, 3U, 0U, 1U});
        clusters.records.push_back({2U, 4U, 0U, 1U});
        clusters.records.push_back({2U, 5U, 1U, 1U});

        caret.flags.resize(
            7U,
            static_cast<std::uint8_t>(kCaretBoundarySafe));
        caret.flags.front() |=
            static_cast<std::uint8_t>(kCaretBoundaryTextEdge);
        caret.flags.back() |=
            static_cast<std::uint8_t>(kCaretBoundaryTextEdge);

        fragments.fragments.push_back(
            {0U, 30U, 0U, 0U, 2U, 0U, 0U, 0U});
        fragments.fragments.push_back(
            {30U,
             70U,
             1U,
             2U,
             4U,
             1U,
             static_cast<std::uint8_t>(
                 kInlineFragmentGlyphRunRtl),
             0U});
        fragments.fragments.push_back(
            {0U, 20U, 2U, 4U, 6U, 0U, 0U, 0U});
        fragments.lines.push_back(
            {100U, 0U, 2U, 4U, kVisualLineContainsRtl});
        fragments.lines.push_back({20U, 2U, 1U, 6U, 0U});

        boxes.fragment_metrics.push_back(
            {0U, 100U, 80U, 0U, 0U});
        boxes.fragment_metrics.push_back(
            {0U, 100U, 80U, 0U, 0U});
        boxes.fragment_metrics.push_back(
            {0U, 100U, 80U, 0U, 0U});
        boxes.lines.push_back(
            {0U, 100U, 80U, 100U, 0U, 2U, 4U, 0U});
        boxes.lines.push_back(
            {100U, 100U, 180U, 20U, 2U, 1U, 6U, 0U});
    }

    ViewportProjectionRequest request() const {
        ViewportProjectionRequest value;
        value.line_boxes = &boxes;
        value.fragment_layout = &fragments;
        value.shaped_text = &shaped;
        value.cluster_map = &clusters;
        value.caret_boundaries = &caret;
        value.viewport_inline_size = 200U;
        value.viewport_block_start = 50U;
        value.viewport_block_size = 50U;
        value.block_overscan = 50U;
        value.selection = {1U, 5U, true};
        value.limits = {8U, 16U, 32U, 16U};
        return value;
    }
};

bool test_projection_and_hit_testing() {
    Fixture fixture;
    ViewportProjection output;
    ViewportProjectionStats stats;
    ViewportProjectionError error;
    const auto request = fixture.request();
    if (!require(
            build_viewport_projection(
                request,
                &output,
                &stats,
                &error),
            "mixed projection succeeds") ||
        !require(
            output.lines.size() == 2U,
            "two overscanned lines projected") ||
        !require(
            output.fragment_rects.size() == 3U,
            "three fragments projected") ||
        !require(
            output.carets.size() == 9U,
            "all safe visual caret edges retained") ||
        !require(
            output.selection_rects.size() == 3U,
            "selection split per fragment") ||
        !require(
            output.lines[0].viewport_block_start == -50,
            "leading overscan is negative") ||
        !require(
            output.lines[1].viewport_block_start == 50,
            "second line is viewport relative") ||
        !require(
            output.carets[2].viewport_inline_position == 30,
            "LTR edge retained") ||
        !require(
            output.carets[3].viewport_inline_position == 30,
            "RTL split caret retained") ||
        !require(
            output.carets[5].viewport_inline_position == 100,
            "RTL logical start is visual end") ||
        !require(
            stats.glyph_groups == 6U,
            "one glyph group per cluster counted")) {
        return false;
    }

    ViewportHitTestResult hit;
    if (!require(
            hit_test_viewport_projection(
                output,
                58,
                0,
                ViewportHitTestBias::Nearest,
                &hit),
            "nearest hit succeeds") ||
        !require(
            hit.boundary_index == 3U,
            "nearest RTL boundary selected")) {
        return false;
    }
    if (!require(
            hit_test_viewport_projection(
                output,
                45,
                0,
                ViewportHitTestBias::TowardVisualStart,
                &hit),
            "visual-start tie hit succeeds") ||
        !require(
            hit.boundary_index == 2U ||
                hit.boundary_index == 4U,
            "visual-start tie uses the 30-unit caret") ||
        !require(
            hit_test_viewport_projection(
                output,
                45,
                0,
                ViewportHitTestBias::TowardVisualEnd,
                &hit),
            "visual-end tie hit succeeds") ||
        !require(
            hit.boundary_index == 3U,
            "visual-end tie uses the 60-unit caret")) {
        return false;
    }
    return true;
}

bool test_horizontal_culling_and_unsafe_boundaries() {
    Fixture fixture;
    fixture.caret.flags[3] = 0U;
    auto request = fixture.request();
    request.viewport_inline_start = 35U;
    request.viewport_inline_size = 20U;
    request.selection.enabled = false;
    ViewportProjection output;
    ViewportProjectionStats stats;
    ViewportProjectionError error;
    if (!require(
            build_viewport_projection(
                request,
                &output,
                &stats,
                &error),
            "horizontally clipped projection succeeds") ||
        !require(
            output.fragment_rects.size() == 1U,
            "only intersecting RTL fragment projected") ||
        !require(
            output.carets.size() == 2U,
            "unsafe boundary omitted") ||
        !require(
            stats.unsafe_caret_boundaries_skipped == 1U,
            "unsafe caret skip is observable")) {
        return false;
    }
    return true;
}

bool test_limits_failure_atomicity_and_budget() {
    Fixture fixture;
    auto request = fixture.request();
    ViewportProjection output;
    ViewportProjectionStats stats;
    ViewportProjectionError error;
    if (!build_viewport_projection(
            request,
            &output,
            &stats,
            &error)) {
        return false;
    }
    request.limits.maximum_carets = 1U;
    if (!require(
            !build_viewport_projection(
                request,
                &output,
                &stats,
                &error),
            "caret limit fails closed") ||
        !require(
            error.kind ==
                ViewportProjectionErrorKind::ProjectionLimitExceeded,
            "limit error is classified") ||
        !require(
            output.lines.empty() && output.carets.empty(),
            "failed rebuild clears stale output")) {
        return false;
    }

    CappedResource capped(1U);
    ViewportProjection tiny(&capped);
    request = fixture.request();
    if (!require(
            !build_viewport_projection(
                request,
                &tiny,
                &stats,
                &error),
            "tiny PMR budget fails") ||
        !require(
            error.kind ==
                ViewportProjectionErrorKind::OutputBudgetExceeded,
            "PMR failure is classified") ||
        !require(
            tiny.lines.empty(),
            "budget failure publishes no records")) {
        return false;
    }
    return true;
}

bool test_selection_requires_safe_endpoints() {
    Fixture fixture;
    fixture.caret.flags[1] = 0U;
    auto request = fixture.request();
    ViewportProjection output;
    ViewportProjectionStats stats;
    ViewportProjectionError error;
    return require(
               !build_viewport_projection(
                   request,
                   &output,
                   &stats,
                   &error),
               "unsafe selection endpoint rejected") &&
           require(
               error.kind == ViewportProjectionErrorKind::InvalidInput,
               "unsafe selection is invalid input");
}

} // namespace

int main() {
    if (!test_projection_and_hit_testing() ||
        !test_horizontal_culling_and_unsafe_boundaries() ||
        !test_limits_failure_atomicity_and_budget() ||
        !test_selection_requires_safe_endpoints()) {
        return 1;
    }
    std::cout << "Viewport projection tests passed\n";
    return 0;
}
