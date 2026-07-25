#include "text_paint_command_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <new>
#include <string>
#include <vector>

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

void configure_segment(
    MultiRunShapedSegment* segment,
    std::uint32_t first_cluster,
    std::uint32_t cluster_limit,
    FontFaceId face_id,
    ShapingDirection direction,
    std::int32_t scale) {
    segment->run.cluster_index = first_cluster;
    segment->run.face_id = face_id;
    segment->run.script = ScriptId::Latn;
    segment->run.direction = direction;
    segment->run.fallback_source = FontFallbackSource::Primary;
    segment->run.bidi_level =
        direction == ShapingDirection::RightToLeft ? 1U : 0U;
    segment->glyphs.first_cluster = first_cluster;
    segment->glyphs.cluster_limit = cluster_limit;
    segment->glyphs.script = ScriptId::Latn;
    segment->glyphs.direction = direction;
    segment->glyphs.x_scale = scale;
    segment->glyphs.y_scale = scale;
}

struct Fixture final {
    MultiRunShapedText shaped;
    GlyphClusterMap clusters;
    LineFragmentLayout fragments;
    ViewportProjection projection;
    std::vector<std::uint32_t> styles;

    Fixture()
        : shaped(std::pmr::get_default_resource()),
          clusters(std::pmr::get_default_resource()),
          fragments(std::pmr::get_default_resource()),
          projection(std::pmr::get_default_resource()) {
        shaped.segments.emplace_back(std::pmr::get_default_resource());
        configure_segment(
            &shaped.segments.back(),
            0U,
            4U,
            1U,
            ShapingDirection::LeftToRight,
            64);
        shaped.segments.back().glyphs.glyphs.push_back(
            {1U, 0U, 10, 0, 0, 0, 0U});
        shaped.segments.back().glyphs.glyphs.push_back(
            {2U, 1U, 20, 0, 0, 0, 0U});
        shaped.segments.back().glyphs.glyphs.push_back(
            {3U, 2U, 15, 0, 0, 0, 0U});
        shaped.segments.back().glyphs.glyphs.push_back(
            {4U, 3U, 15, 0, 0, 0, 0U});

        shaped.segments.emplace_back(std::pmr::get_default_resource());
        configure_segment(
            &shaped.segments.back(),
            4U,
            6U,
            2U,
            ShapingDirection::RightToLeft,
            72);
        shaped.segments.back().glyphs.glyphs.push_back(
            {5U, 5U, -25, 0, 0, 0, 0U});
        shaped.segments.back().glyphs.glyphs.push_back(
            {6U, 4U, -35, 0, 0, 0, 0U});

        shaped.segments.emplace_back(std::pmr::get_default_resource());
        configure_segment(
            &shaped.segments.back(),
            6U,
            7U,
            3U,
            ShapingDirection::LeftToRight,
            64);

        clusters.records.push_back({0U, 0U, 0U, 1U});
        clusters.records.push_back({0U, 1U, 1U, 1U});
        clusters.records.push_back({0U, 2U, 2U, 1U});
        clusters.records.push_back({0U, 3U, 3U, 1U});
        clusters.records.push_back({1U, 4U, 1U, 1U});
        clusters.records.push_back({1U, 5U, 0U, 1U});
        clusters.records.push_back({2U, 6U, 0U, 0U});

        fragments.fragments.push_back(
            {0U, 30U, 0U, 0U, 2U, 0U, 0U, 0U});
        fragments.fragments.push_back(
            {30U, 30U, 0U, 2U, 4U, 0U, 0U, 0U});
        fragments.fragments.push_back(
            {60U,
             60U,
             1U,
             4U,
             6U,
             1U,
             static_cast<std::uint8_t>(
                 kInlineFragmentGlyphRunRtl),
             0U});
        fragments.fragments.push_back(
            {0U,
             0U,
             2U,
             6U,
             7U,
             0U,
             static_cast<std::uint8_t>(
                 kInlineFragmentContainsX9Only),
             0U});
        fragments.lines.push_back(
            {120U, 0U, 3U, 6U, kVisualLineContainsRtl});
        fragments.lines.push_back(
            {0U, 3U, 1U, 7U, kVisualLineContainsX9Only});

        projection.viewport_inline_start = 0U;
        projection.viewport_block_start = 20U;
        projection.document_block_extent = 200U;
        projection.fragment_rects.push_back(
            {0, -20, 30U, 100U, 0U, 0U, 2U, 0U});
        projection.fragment_rects.push_back(
            {30, -20, 30U, 100U, 1U, 2U, 4U, 0U});
        projection.fragment_rects.push_back(
            {60,
             -20,
             60U,
             100U,
             2U,
             4U,
             6U,
             kViewportFragmentRtl});
        projection.fragment_rects.push_back(
            {0,
             80,
             0U,
             100U,
             3U,
             6U,
             7U,
             kViewportFragmentContainsX9Only});

        projection.selection_rects.push_back(
            {70,
             -20,
             20U,
             100U,
             0U,
             2U,
             kViewportSelectionRtl,
             0U});
        projection.selection_rects.push_back(
            {0, -20, 0U, 100U, 0U, 0U, 0U, 0U});

        projection.carets.push_back(
            {0, -20, 100U, 0U, 0U, 0U, 0U});
        projection.carets.push_back(
            {95,
             -20,
             100U,
             5U,
             2U,
             kViewportCaretRtl,
             0U});

        projection.lines.push_back(
            {-20,
             60,
             100U,
             120U,
             0U,
             0U,
             3U,
             0U,
             2U,
             0U,
             2U,
             kViewportLineContainsSelection |
                 kViewportLineContainsRtl |
                 kViewportLineBeforeViewport});
        projection.lines.push_back(
            {80,
             160,
             100U,
             0U,
             1U,
             3U,
             1U,
             2U,
             0U,
             2U,
             0U,
             kViewportLineAfterViewport});

        styles = {11U, 22U, 33U};
    }

    TextPaintCommandStreamRequest request() const {
        TextPaintCommandStreamRequest value;
        value.projection = &projection;
        value.fragment_layout = &fragments;
        value.shaped_text = &shaped;
        value.cluster_map = &clusters;
        value.segment_style_ids = styles;
        value.selection_style_id = 44U;
        value.caret_style_id = 55U;
        value.clip_inline_size = 200U;
        value.clip_block_size = 100U;
        value.paint_selection = true;
        value.caret = {0U, 2U, 5U, 0U, 2U, true};
        value.limits = {16U, 8U, 8U, 16U};
        return value;
    }
};

bool test_command_order_and_payloads() {
    Fixture fixture;
    TextPaintCommandStream output;
    TextPaintCommandStreamStats stats;
    TextPaintCommandStreamError error;
    const auto request = fixture.request();
    if (!require(
            build_text_paint_command_stream(
                request,
                &output,
                &stats,
                &error),
            "paint stream succeeds") ||
        !require(output.clips.size() == 1U, "one viewport clip") ||
        !require(output.commands.size() == 4U, "four commands") ||
        !require(
            output.glyph_batches.size() == 2U,
            "LTR fragments coalesced and RTL retained") ||
        !require(output.fill_rects.size() == 2U, "selection and caret rects") ||
        !require(
            output.commands[0].kind ==
                TextPaintCommandKind::SelectionRect &&
            output.commands[1].kind ==
                TextPaintCommandKind::GlyphBatch &&
            output.commands[2].kind ==
                TextPaintCommandKind::GlyphBatch &&
            output.commands[3].kind ==
                TextPaintCommandKind::CaretRect,
            "strict selection-glyph-caret order")) {
        return false;
    }

    const TextPaintGlyphBatch& ltr = output.glyph_batches[0];
    const TextPaintGlyphBatch& rtl = output.glyph_batches[1];
    if (!require(
            ltr.first_glyph == 0U && ltr.glyph_count == 4U &&
                ltr.source_fragment_count == 2U &&
                ltr.viewport_inline_origin == 0 &&
                ltr.style_id == 11U &&
                (ltr.flags & kTextPaintGlyphBatchCoalesced) != 0U,
            "safe LTR spans coalesced") ||
        !require(
            rtl.first_glyph == 0U && rtl.glyph_count == 2U &&
                rtl.viewport_inline_origin == 120 &&
                rtl.style_id == 22U &&
                (rtl.flags & kTextPaintGlyphBatchRtl) != 0U,
            "RTL origin uses visual fragment end") ||
        !require(
            output.fill_rects[0].style_id == 44U &&
                (output.fill_rects[0].flags &
                 kTextPaintRectSelection) != 0U,
            "selection style retained") ||
        !require(
            output.fill_rects[1].style_id == 55U &&
                output.fill_rects[1].inline_size == 2U &&
                output.fill_rects[1].viewport_inline_start == 95 &&
                (output.fill_rects[1].flags &
                 kTextPaintRectCaret) != 0U,
            "active caret retained")) {
        return false;
    }

    return require(
               stats.selection_commands == 1U &&
                   stats.caret_commands == 1U &&
                   stats.referenced_glyphs == 6U &&
                   stats.coalesced_fragments == 1U &&
                   stats.zero_glyph_fragments_skipped == 1U &&
                   stats.zero_area_selection_rects_skipped == 1U &&
                   stats.rtl_glyph_batches == 1U,
               "paint stats match certified fixture") &&
           require(
               (output.commands[0].flags &
                kTextPaintCommandBeforeViewport) != 0U,
               "overscan paint command is classified");
}

bool test_fail_closed_contracts() {
    Fixture fixture;
    TextPaintCommandStream output;
    TextPaintCommandStreamStats stats;
    TextPaintCommandStreamError error;

    auto request = fixture.request();
    request.limits.maximum_commands = 3U;
    if (!require(
            !build_text_paint_command_stream(
                request,
                &output,
                &stats,
                &error),
            "command limit rejects output") ||
        !require(
            error.kind ==
                TextPaintCommandStreamErrorKind::CommandLimitExceeded,
            "command limit error classified") ||
        !require(output.commands.empty(), "limit failure leaves output empty")) {
        return false;
    }

    request = fixture.request();
    request.caret.boundary_index = 4U;
    if (!require(
            !build_text_paint_command_stream(
                request,
                &output,
                &stats,
                &error),
            "missing exact caret rejected") ||
        !require(
            error.kind ==
                TextPaintCommandStreamErrorKind::CaretNotFound,
            "caret error classified")) {
        return false;
    }

    request = fixture.request();
    fixture.projection.fragment_rects[0].inline_size = 29U;
    if (!require(
            !build_text_paint_command_stream(
                request,
                &output,
                &stats,
                &error),
            "advance mismatch rejected") ||
        !require(
            error.kind ==
                TextPaintCommandStreamErrorKind::AdvanceMismatch,
            "advance error classified")) {
        return false;
    }
    fixture.projection.fragment_rects[0].inline_size = 30U;

    request = fixture.request();
    request.segment_style_ids = std::span<const std::uint32_t>(
        fixture.styles.data(),
        2U);
    if (!require(
            !build_text_paint_command_stream(
                request,
                &output,
                &stats,
                &error),
            "short style table rejected") ||
        !require(
            error.kind ==
                TextPaintCommandStreamErrorKind::InvalidInput,
            "style error classified")) {
        return false;
    }

    CappedResource capped(64U);
    TextPaintCommandStream capped_output(&capped);
    request = fixture.request();
    if (!require(
            !build_text_paint_command_stream(
                request,
                &capped_output,
                &stats,
                &error),
            "PMR rejection fails closed") ||
        !require(
            error.kind ==
                TextPaintCommandStreamErrorKind::OutputBudgetExceeded,
            "PMR failure classified") ||
        !require(
            capped_output.commands.empty() &&
                capped_output.glyph_batches.empty() &&
                capped_output.fill_rects.empty(),
            "PMR failure clears partial publication")) {
        return false;
    }

    output.commands.push_back(
        {TextPaintCommandKind::CaretRect, 0U, 0U, 0U});
    request = fixture.request();
    request.projection = nullptr;
    return require(
               !build_text_paint_command_stream(
                   request,
                   &output,
                   &stats,
                   &error),
               "invalid request rejected") &&
           require(
               output.commands.empty() &&
                   output.glyph_batches.empty() &&
                   output.fill_rects.empty(),
               "stale output cleared before validation");
}

} // namespace

int main() {
    if (!test_command_order_and_payloads() ||
        !test_fail_closed_contracts()) {
        return 1;
    }
    std::cout << "text paint command stream tests passed\n";
    return 0;
}
