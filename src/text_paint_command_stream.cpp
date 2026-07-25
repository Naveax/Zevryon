#include "text_paint_command_stream.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace zevryon::text {
namespace {

template <typename T>
void release_vector(std::pmr::vector<T>* values) noexcept {
    std::pmr::vector<T> empty(values->get_allocator().resource());
    values->swap(empty);
}

void clear_error(TextPaintCommandStreamError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
}

bool fail(
    TextPaintCommandStreamErrorKind kind,
    std::size_t line_index,
    std::size_t fragment_index,
    std::size_t glyph_index,
    std::uint32_t cluster_index,
    const char* message,
    TextPaintCommandStreamError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->line_index = line_index;
        error->fragment_index = fragment_index;
        error->glyph_index = glyph_index;
        error->cluster_index = cluster_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool checked_add_u64(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t* output) noexcept {
    if (output == nullptr ||
        right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    *output = left + right;
    return true;
}

bool checked_add_u32(
    std::uint32_t left,
    std::uint32_t right,
    std::uint32_t* output) noexcept {
    if (output == nullptr ||
        right > std::numeric_limits<std::uint32_t>::max() - left) {
        return false;
    }
    *output = left + right;
    return true;
}

bool checked_add_i64_i32(
    std::int64_t left,
    std::int32_t right,
    std::int64_t* output) noexcept {
    if (output == nullptr) {
        return false;
    }
    const std::int64_t widened = right;
    if ((widened > 0 &&
         left > std::numeric_limits<std::int64_t>::max() - widened) ||
        (widened < 0 &&
         left < std::numeric_limits<std::int64_t>::min() - widened)) {
        return false;
    }
    *output = left + widened;
    return true;
}

bool checked_add_signed_size(
    std::int64_t start,
    std::uint64_t size,
    std::int64_t* output) noexcept {
    if (output == nullptr ||
        size > static_cast<std::uint64_t>(
                   std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    const std::int64_t signed_size = static_cast<std::int64_t>(size);
    if (start > std::numeric_limits<std::int64_t>::max() - signed_size) {
        return false;
    }
    *output = start + signed_size;
    return true;
}

std::uint64_t absolute_i64(std::int64_t value) noexcept {
    if (value >= 0) {
        return static_cast<std::uint64_t>(value);
    }
    return static_cast<std::uint64_t>(-(value + 1)) + 1U;
}

bool same_group(
    const GlyphClusterRecord& left,
    const GlyphClusterRecord& right) noexcept {
    return left.segment_index == right.segment_index &&
           left.owner_cluster == right.owner_cluster &&
           left.first_glyph == right.first_glyph &&
           left.glyph_count == right.glyph_count;
}

std::uint32_t style_for_segment(
    const TextPaintCommandStreamRequest& request,
    std::uint32_t segment_index) noexcept {
    return request.segment_style_ids.empty()
        ? request.default_text_style_id
        : request.segment_style_ids[segment_index];
}

std::uint32_t command_flags_from_line(
    const ViewportLineRecord& line) noexcept {
    std::uint32_t flags = 0U;
    if ((line.flags & kViewportLineBeforeViewport) != 0U) {
        flags |= kTextPaintCommandBeforeViewport;
    }
    if ((line.flags & kViewportLineAfterViewport) != 0U) {
        flags |= kTextPaintCommandAfterViewport;
    }
    return flags;
}

const ViewportLineRecord* find_projection_line(
    const ViewportProjection& projection,
    std::uint32_t source_line_index) noexcept {
    const auto iterator = std::lower_bound(
        projection.lines.begin(),
        projection.lines.end(),
        source_line_index,
        [](const ViewportLineRecord& line, std::uint32_t value) {
            return line.source_line_index < value;
        });
    return iterator != projection.lines.end() &&
                   iterator->source_line_index == source_line_index
        ? &*iterator
        : nullptr;
}

struct ResolvedGlyphSpan final {
    std::uint32_t first_glyph{0};
    std::uint32_t glyph_count{0};
    std::int64_t signed_advance{0};
    bool has_glyphs{false};
};

bool resolve_glyph_span(
    const TextPaintCommandStreamRequest& request,
    const InlineLayoutFragment& fragment,
    const ViewportFragmentRect& rect,
    std::size_t line_index,
    std::size_t fragment_index,
    ResolvedGlyphSpan* output,
    TextPaintCommandStreamError* error) noexcept {
    *output = {};
    if (fragment.segment_index >= request.shaped_text->segments.size() ||
        fragment.first_cluster >= fragment.cluster_limit ||
        fragment.cluster_limit > request.cluster_map->records.size()) {
        return fail(
            TextPaintCommandStreamErrorKind::TopologyViolation,
            line_index,
            fragment_index,
            0U,
            fragment.first_cluster,
            "source fragment is outside the retained shaped or cluster domain",
            error);
    }

    const MultiRunShapedSegment& segment =
        request.shaped_text->segments[fragment.segment_index];
    const bool rtl =
        (fragment.flags & kInlineFragmentGlyphRunRtl) != 0U;
    if ((rtl &&
         segment.glyphs.direction != ShapingDirection::RightToLeft) ||
        (!rtl &&
         segment.glyphs.direction != ShapingDirection::LeftToRight) ||
        segment.run.direction != segment.glyphs.direction ||
        segment.run.face_id == kInvalidFontFaceId ||
        segment.glyphs.x_scale <= 0 ||
        segment.glyphs.y_scale <= 0) {
        return fail(
            TextPaintCommandStreamErrorKind::TopologyViolation,
            line_index,
            fragment_index,
            0U,
            fragment.first_cluster,
            "fragment direction, face, or retained scale is invalid",
            error);
    }

    std::uint64_t unique_glyphs = 0U;
    std::uint32_t minimum_glyph =
        std::numeric_limits<std::uint32_t>::max();
    std::uint32_t maximum_glyph_limit = 0U;
    GlyphClusterRecord previous{};
    bool have_previous = false;

    for (std::uint32_t cluster = fragment.first_cluster;
         cluster < fragment.cluster_limit;
         ++cluster) {
        const GlyphClusterRecord& record =
            request.cluster_map->records[cluster];
        if (record.segment_index != fragment.segment_index ||
            record.owner_cluster < fragment.first_cluster ||
            record.owner_cluster >= fragment.cluster_limit) {
            return fail(
                TextPaintCommandStreamErrorKind::TopologyViolation,
                line_index,
                fragment_index,
                0U,
                cluster,
                "glyph owner escapes the source fragment",
                error);
        }
        if (have_previous && same_group(previous, record)) {
            continue;
        }
        previous = record;
        have_previous = true;

        if (record.glyph_count == 0U) {
            continue;
        }
        if (record.first_glyph > segment.glyphs.glyphs.size() ||
            record.glyph_count >
                segment.glyphs.glyphs.size() - record.first_glyph) {
            return fail(
                TextPaintCommandStreamErrorKind::MissingGlyphSpan,
                line_index,
                fragment_index,
                record.first_glyph,
                cluster,
                "glyph owner span is outside the shaped segment",
                error);
        }

        std::uint32_t glyph_limit = 0U;
        if (!checked_add_u32(
                record.first_glyph,
                record.glyph_count,
                &glyph_limit) ||
            !checked_add_u64(
                unique_glyphs,
                record.glyph_count,
                &unique_glyphs)) {
            return fail(
                TextPaintCommandStreamErrorKind::AggregateOverflow,
                line_index,
                fragment_index,
                record.first_glyph,
                cluster,
                "glyph-span aggregate overflow",
                error);
        }
        minimum_glyph = std::min(minimum_glyph, record.first_glyph);
        maximum_glyph_limit =
            std::max(maximum_glyph_limit, glyph_limit);
    }

    if (unique_glyphs == 0U) {
        if (rect.inline_size != 0U) {
            return fail(
                TextPaintCommandStreamErrorKind::AdvanceMismatch,
                line_index,
                fragment_index,
                0U,
                fragment.first_cluster,
                "zero-glyph fragment retains non-zero inline geometry",
                error);
        }
        return true;
    }
    if (minimum_glyph == std::numeric_limits<std::uint32_t>::max() ||
        maximum_glyph_limit < minimum_glyph ||
        static_cast<std::uint64_t>(
            maximum_glyph_limit - minimum_glyph) != unique_glyphs) {
        return fail(
            TextPaintCommandStreamErrorKind::MissingGlyphSpan,
            line_index,
            fragment_index,
            minimum_glyph,
            fragment.first_cluster,
            "fragment glyph owners do not form one contiguous zero-copy span",
            error);
    }

    std::int64_t signed_advance = 0;
    for (std::uint32_t glyph = minimum_glyph;
         glyph < maximum_glyph_limit;
         ++glyph) {
        if (!checked_add_i64_i32(
                signed_advance,
                segment.glyphs.glyphs[glyph].x_advance,
                &signed_advance)) {
            return fail(
                TextPaintCommandStreamErrorKind::ArithmeticOverflow,
                line_index,
                fragment_index,
                glyph,
                fragment.first_cluster,
                "signed glyph advance overflow",
                error);
        }
    }
    if (absolute_i64(signed_advance) != rect.inline_size ||
        (!rtl && signed_advance < 0) ||
        (rtl && signed_advance > 0)) {
        return fail(
            TextPaintCommandStreamErrorKind::AdvanceMismatch,
            line_index,
            fragment_index,
            minimum_glyph,
            fragment.first_cluster,
            "glyph-span advance does not match projected fragment geometry",
            error);
    }

    output->first_glyph = minimum_glyph;
    output->glyph_count =
        maximum_glyph_limit - minimum_glyph;
    output->signed_advance = signed_advance;
    output->has_glyphs = true;
    return true;
}

struct GlyphBatchCandidate final {
    TextPaintGlyphBatch batch;
    std::uint64_t inline_size{0};
};

bool build_candidate(
    const TextPaintCommandStreamRequest& request,
    const ViewportLineRecord& line,
    const ViewportFragmentRect& rect,
    std::size_t projected_line_index,
    std::size_t projected_fragment_index,
    GlyphBatchCandidate* output,
    bool* has_output,
    TextPaintCommandStreamError* error) noexcept {
    *output = {};
    *has_output = false;

    if (line.source_line_index >= request.fragment_layout->lines.size() ||
        rect.source_fragment_index >=
            request.fragment_layout->fragments.size()) {
        return fail(
            TextPaintCommandStreamErrorKind::TopologyViolation,
            projected_line_index,
            projected_fragment_index,
            0U,
            rect.first_cluster,
            "projection references a missing source line or fragment",
            error);
    }
    const VisualLineLayoutRecord& source_line =
        request.fragment_layout->lines[line.source_line_index];
    const std::uint64_t source_fragment_limit =
        static_cast<std::uint64_t>(source_line.first_fragment) +
        source_line.fragment_count;
    if (rect.source_fragment_index < source_line.first_fragment ||
        rect.source_fragment_index >= source_fragment_limit) {
        return fail(
            TextPaintCommandStreamErrorKind::TopologyViolation,
            projected_line_index,
            projected_fragment_index,
            0U,
            rect.first_cluster,
            "projected fragment is outside its source line slice",
            error);
    }

    const InlineLayoutFragment& fragment =
        request.fragment_layout->fragments[
            rect.source_fragment_index];
    if (fragment.first_cluster != rect.first_cluster ||
        fragment.cluster_limit != rect.cluster_limit) {
        return fail(
            TextPaintCommandStreamErrorKind::TopologyViolation,
            projected_line_index,
            projected_fragment_index,
            0U,
            rect.first_cluster,
            "projected cluster range diverges from the source fragment",
            error);
    }

    ResolvedGlyphSpan span;
    if (!resolve_glyph_span(
            request,
            fragment,
            rect,
            projected_line_index,
            projected_fragment_index,
            &span,
            error)) {
        return false;
    }
    if (!span.has_glyphs) {
        return true;
    }

    const MultiRunShapedSegment& segment =
        request.shaped_text->segments[fragment.segment_index];
    const bool rtl =
        (fragment.flags & kInlineFragmentGlyphRunRtl) != 0U;
    std::int64_t origin = rect.viewport_inline_start;
    if (rtl &&
        !checked_add_signed_size(
            rect.viewport_inline_start,
            rect.inline_size,
            &origin)) {
        return fail(
            TextPaintCommandStreamErrorKind::ArithmeticOverflow,
            projected_line_index,
            projected_fragment_index,
            span.first_glyph,
            fragment.first_cluster,
            "RTL glyph origin exceeds the signed viewport contract",
            error);
    }

    std::uint32_t flags = 0U;
    if (rtl) {
        flags |= kTextPaintGlyphBatchRtl;
    }
    if ((fragment.flags & kInlineFragmentL1Adjusted) != 0U) {
        flags |= kTextPaintGlyphBatchL1Adjusted;
    }
    if ((line.flags & kViewportLineBeforeViewport) != 0U) {
        flags |= kTextPaintGlyphBatchBeforeViewport;
    }
    if ((line.flags & kViewportLineAfterViewport) != 0U) {
        flags |= kTextPaintGlyphBatchAfterViewport;
    }
    if ((rect.flags &
         kViewportFragmentBeforeInlineViewport) != 0U) {
        flags |= kTextPaintGlyphBatchBeforeInlineViewport;
    }
    if ((rect.flags &
         kViewportFragmentAfterInlineViewport) != 0U) {
        flags |= kTextPaintGlyphBatchAfterInlineViewport;
    }

    output->batch = {
        origin,
        line.viewport_baseline,
        fragment.segment_index,
        span.first_glyph,
        span.glyph_count,
        style_for_segment(request, fragment.segment_index),
        segment.run.face_id,
        segment.glyphs.x_scale,
        segment.glyphs.y_scale,
        line.source_line_index,
        rect.source_fragment_index,
        1U,
        flags,
        0U};
    output->inline_size = rect.inline_size;
    *has_output = true;
    return true;
}

bool can_coalesce(
    const GlyphBatchCandidate& previous,
    const GlyphBatchCandidate& current) noexcept {
    if ((previous.batch.flags & kTextPaintGlyphBatchRtl) != 0U ||
        (current.batch.flags & kTextPaintGlyphBatchRtl) != 0U ||
        previous.batch.segment_index != current.batch.segment_index ||
        previous.batch.style_id != current.batch.style_id ||
        previous.batch.face_id != current.batch.face_id ||
        previous.batch.x_scale != current.batch.x_scale ||
        previous.batch.y_scale != current.batch.y_scale ||
        previous.batch.source_line_index !=
            current.batch.source_line_index ||
        previous.batch.viewport_baseline !=
            current.batch.viewport_baseline) {
        return false;
    }

    std::uint32_t glyph_limit = 0U;
    std::uint32_t fragment_limit = 0U;
    std::int64_t expected_origin = 0;
    return checked_add_u32(
               previous.batch.first_glyph,
               previous.batch.glyph_count,
               &glyph_limit) &&
           glyph_limit == current.batch.first_glyph &&
           checked_add_u32(
               previous.batch.first_source_fragment_index,
               previous.batch.source_fragment_count,
               &fragment_limit) &&
           fragment_limit ==
               current.batch.first_source_fragment_index &&
           checked_add_signed_size(
               previous.batch.viewport_inline_origin,
               previous.inline_size,
               &expected_origin) &&
           expected_origin == current.batch.viewport_inline_origin;
}

bool merge_candidate(
    GlyphBatchCandidate* previous,
    const GlyphBatchCandidate& current) noexcept {
    std::uint32_t glyph_count = 0U;
    std::uint32_t fragment_count = 0U;
    std::uint64_t inline_size = 0U;
    if (!checked_add_u32(
            previous->batch.glyph_count,
            current.batch.glyph_count,
            &glyph_count) ||
        !checked_add_u32(
            previous->batch.source_fragment_count,
            current.batch.source_fragment_count,
            &fragment_count) ||
        !checked_add_u64(
            previous->inline_size,
            current.inline_size,
            &inline_size)) {
        return false;
    }
    previous->batch.glyph_count = glyph_count;
    previous->batch.source_fragment_count = fragment_count;
    previous->batch.flags |=
        current.batch.flags | kTextPaintGlyphBatchCoalesced;
    previous->inline_size = inline_size;
    return true;
}

struct PaintCounts final {
    std::uint64_t selection_rects{0};
    std::uint64_t glyph_batches{0};
    std::uint64_t caret_rects{0};
    std::uint64_t commands{0};
    std::uint64_t fill_rects{0};
    std::uint64_t referenced_glyphs{0};
};

bool flush_candidate(
    bool emit,
    const GlyphBatchCandidate& candidate,
    TextPaintCommandStream* output,
    PaintCounts* counts,
    TextPaintCommandStreamStats* stats,
    TextPaintCommandStreamError* error,
    std::size_t line_index,
    std::uint64_t* line_batches) noexcept {
    if (!checked_add_u64(
            counts->glyph_batches,
            1U,
            &counts->glyph_batches) ||
        !checked_add_u64(
            counts->commands,
            1U,
            &counts->commands) ||
        !checked_add_u64(
            counts->referenced_glyphs,
            candidate.batch.glyph_count,
            &counts->referenced_glyphs) ||
        !checked_add_u64(*line_batches, 1U, line_batches)) {
        return fail(
            TextPaintCommandStreamErrorKind::AggregateOverflow,
            line_index,
            candidate.batch.first_source_fragment_index,
            candidate.batch.first_glyph,
            0U,
            "paint glyph-batch aggregate overflow",
            error);
    }

    if (stats != nullptr) {
        if ((candidate.batch.flags & kTextPaintGlyphBatchRtl) != 0U) {
            ++stats->rtl_glyph_batches;
        }
        stats->maximum_glyphs_per_batch =
            std::max<std::uint64_t>(
                stats->maximum_glyphs_per_batch,
                candidate.batch.glyph_count);
    }

    if (emit) {
        const std::uint32_t payload_index =
            static_cast<std::uint32_t>(
                output->glyph_batches.size());
        output->glyph_batches.push_back(candidate.batch);
        output->commands.push_back({
            TextPaintCommandKind::GlyphBatch,
            payload_index,
            0U,
            (candidate.batch.flags &
             kTextPaintGlyphBatchBeforeViewport) != 0U
                ? kTextPaintCommandBeforeViewport
                : (candidate.batch.flags &
                   kTextPaintGlyphBatchAfterViewport) != 0U
                    ? kTextPaintCommandAfterViewport
                    : 0U});
    }
    return true;
}

bool process_glyph_batches(
    bool emit,
    const TextPaintCommandStreamRequest& request,
    TextPaintCommandStream* output,
    PaintCounts* counts,
    TextPaintCommandStreamStats* stats,
    TextPaintCommandStreamError* error) noexcept {
    for (std::size_t line_index = 0U;
         line_index < request.projection->lines.size();
         ++line_index) {
        const ViewportLineRecord& line =
            request.projection->lines[line_index];
        const std::uint64_t fragment_limit =
            static_cast<std::uint64_t>(line.first_fragment_rect) +
            line.fragment_rect_count;
        if (fragment_limit >
            request.projection->fragment_rects.size()) {
            return fail(
                TextPaintCommandStreamErrorKind::TopologyViolation,
                line_index,
                line.first_fragment_rect,
                0U,
                0U,
                "projected fragment slice is outside the projection",
                error);
        }

        GlyphBatchCandidate pending;
        bool have_pending = false;
        std::uint64_t line_batches = 0U;
        for (std::uint32_t offset = 0U;
             offset < line.fragment_rect_count;
             ++offset) {
            const std::size_t fragment_index =
                static_cast<std::size_t>(
                    line.first_fragment_rect + offset);
            const ViewportFragmentRect& rect =
                request.projection->fragment_rects[
                    fragment_index];
            GlyphBatchCandidate current;
            bool has_current = false;
            if (!build_candidate(
                    request,
                    line,
                    rect,
                    line_index,
                    fragment_index,
                    &current,
                    &has_current,
                    error)) {
                return false;
            }
            if (!has_current) {
                if (stats != nullptr) {
                    ++stats->zero_glyph_fragments_skipped;
                }
                continue;
            }
            if (have_pending && can_coalesce(pending, current)) {
                if (!merge_candidate(&pending, current)) {
                    return fail(
                        TextPaintCommandStreamErrorKind::AggregateOverflow,
                        line_index,
                        fragment_index,
                        current.batch.first_glyph,
                        rect.first_cluster,
                        "coalesced glyph batch overflow",
                        error);
                }
                if (stats != nullptr) {
                    ++stats->coalesced_fragments;
                }
                continue;
            }
            if (have_pending &&
                !flush_candidate(
                    emit,
                    pending,
                    output,
                    counts,
                    stats,
                    error,
                    line_index,
                    &line_batches)) {
                return false;
            }
            pending = current;
            have_pending = true;
        }
        if (have_pending &&
            !flush_candidate(
                emit,
                pending,
                output,
                counts,
                stats,
                error,
                line_index,
                &line_batches)) {
            return false;
        }
        if (stats != nullptr) {
            stats->maximum_glyph_batches_per_line =
                std::max(
                    stats->maximum_glyph_batches_per_line,
                    line_batches);
        }
    }
    return true;
}

bool process_selection_rects(
    bool emit,
    const TextPaintCommandStreamRequest& request,
    TextPaintCommandStream* output,
    PaintCounts* counts,
    TextPaintCommandStreamStats* stats,
    TextPaintCommandStreamError* error) noexcept {
    if (!request.paint_selection) {
        return true;
    }

    for (std::size_t index = 0U;
         index < request.projection->selection_rects.size();
         ++index) {
        const ViewportSelectionRect& source =
            request.projection->selection_rects[index];
        const ViewportLineRecord* line =
            find_projection_line(
                *request.projection,
                source.source_line_index);
        if (line == nullptr ||
            source.source_fragment_index >=
                request.fragment_layout->fragments.size() ||
            source.inline_size >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()) ||
            source.block_size >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
            return fail(
                TextPaintCommandStreamErrorKind::TopologyViolation,
                source.source_line_index,
                source.source_fragment_index,
                0U,
                0U,
                "selection rectangle references invalid source geometry",
                error);
        }
        if (source.inline_size == 0U || source.block_size == 0U) {
            if (stats != nullptr) {
                ++stats->zero_area_selection_rects_skipped;
            }
            continue;
        }

        if (!checked_add_u64(
                counts->selection_rects,
                1U,
                &counts->selection_rects) ||
            !checked_add_u64(
                counts->fill_rects,
                1U,
                &counts->fill_rects) ||
            !checked_add_u64(
                counts->commands,
                1U,
                &counts->commands)) {
            return fail(
                TextPaintCommandStreamErrorKind::AggregateOverflow,
                source.source_line_index,
                source.source_fragment_index,
                0U,
                0U,
                "selection command aggregate overflow",
                error);
        }

        if (emit) {
            std::uint32_t flags = kTextPaintRectSelection;
            if ((source.flags & kViewportSelectionRtl) != 0U) {
                flags |= kTextPaintRectRtl;
            }
            if ((source.flags &
                 kViewportSelectionStartsInsideFragment) != 0U) {
                flags |= kTextPaintRectStartsInsideFragment;
            }
            if ((source.flags &
                 kViewportSelectionEndsInsideFragment) != 0U) {
                flags |= kTextPaintRectEndsInsideFragment;
            }
            const std::uint32_t payload_index =
                static_cast<std::uint32_t>(
                    output->fill_rects.size());
            output->fill_rects.push_back({
                source.viewport_inline_start,
                source.viewport_block_start,
                source.inline_size,
                source.block_size,
                request.selection_style_id,
                source.source_line_index,
                source.source_fragment_index,
                flags});
            output->commands.push_back({
                TextPaintCommandKind::SelectionRect,
                payload_index,
                0U,
                command_flags_from_line(*line)});
        }
    }
    return true;
}

bool process_caret(
    bool emit,
    const TextPaintCommandStreamRequest& request,
    TextPaintCommandStream* output,
    PaintCounts* counts,
    TextPaintCommandStreamStats*,
    TextPaintCommandStreamError* error) noexcept {
    if (!request.caret.enabled) {
        return true;
    }
    if (request.caret.inline_size == 0U ||
        request.caret.inline_size >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
        return fail(
            TextPaintCommandStreamErrorKind::InvalidInput,
            request.caret.source_line_index,
            request.caret.source_fragment_index,
            0U,
            request.caret.boundary_index,
            "caret inline size is outside the paint contract",
            error);
    }

    const ViewportCaretEdge* matched = nullptr;
    const ViewportLineRecord* matched_line = nullptr;
    std::uint64_t matches = 0U;
    for (const ViewportLineRecord& line :
         request.projection->lines) {
        const std::uint64_t caret_limit =
            static_cast<std::uint64_t>(line.first_caret) +
            line.caret_count;
        if (caret_limit > request.projection->carets.size()) {
            return fail(
                TextPaintCommandStreamErrorKind::TopologyViolation,
                line.source_line_index,
                0U,
                0U,
                0U,
                "projected caret slice is outside the projection",
                error);
        }
        if (line.source_line_index !=
            request.caret.source_line_index) {
            continue;
        }
        for (std::uint32_t offset = 0U;
             offset < line.caret_count;
             ++offset) {
            const ViewportCaretEdge& candidate =
                request.projection->carets[
                    line.first_caret + offset];
            if (candidate.source_fragment_index ==
                    request.caret.source_fragment_index &&
                candidate.boundary_index ==
                    request.caret.boundary_index) {
                matched = &candidate;
                matched_line = &line;
                ++matches;
            }
        }
    }
    if (matches != 1U || matched == nullptr ||
        matched_line == nullptr) {
        return fail(
            TextPaintCommandStreamErrorKind::CaretNotFound,
            request.caret.source_line_index,
            request.caret.source_fragment_index,
            0U,
            request.caret.boundary_index,
            "active caret selector did not resolve to exactly one projected edge",
            error);
    }

    if (!checked_add_u64(
            counts->caret_rects,
            1U,
            &counts->caret_rects) ||
        !checked_add_u64(
            counts->fill_rects,
            1U,
            &counts->fill_rects) ||
        !checked_add_u64(
            counts->commands,
            1U,
            &counts->commands)) {
        return fail(
            TextPaintCommandStreamErrorKind::AggregateOverflow,
            request.caret.source_line_index,
            request.caret.source_fragment_index,
            0U,
            request.caret.boundary_index,
            "caret command aggregate overflow",
            error);
    }

    if (emit) {
        std::uint32_t flags = kTextPaintRectCaret;
        if ((matched->flags & kViewportCaretRtl) != 0U) {
            flags |= kTextPaintRectRtl;
        }
        const std::uint32_t payload_index =
            static_cast<std::uint32_t>(
                output->fill_rects.size());
        output->fill_rects.push_back({
            matched->viewport_inline_position,
            matched->viewport_block_start,
            request.caret.inline_size,
            matched->block_size,
            request.caret_style_id,
            request.caret.source_line_index,
            request.caret.source_fragment_index,
            flags});
        output->commands.push_back({
            TextPaintCommandKind::CaretRect,
            payload_index,
            0U,
            command_flags_from_line(*matched_line)});
    }
    return true;
}

bool validate_projection_topology(
    const TextPaintCommandStreamRequest& request,
    TextPaintCommandStreamError* error) noexcept {
    std::uint32_t previous_source_line = 0U;
    bool have_previous = false;
    for (std::size_t index = 0U;
         index < request.projection->lines.size();
         ++index) {
        const ViewportLineRecord& line =
            request.projection->lines[index];
        if ((have_previous &&
             line.source_line_index <= previous_source_line) ||
            line.source_line_index >=
                request.fragment_layout->lines.size()) {
            return fail(
                TextPaintCommandStreamErrorKind::TopologyViolation,
                index,
                line.first_fragment_rect,
                0U,
                0U,
                "projected source lines are not strictly increasing",
                error);
        }
        previous_source_line = line.source_line_index;
        have_previous = true;

        const std::uint64_t fragment_limit =
            static_cast<std::uint64_t>(line.first_fragment_rect) +
            line.fragment_rect_count;
        const std::uint64_t caret_limit =
            static_cast<std::uint64_t>(line.first_caret) +
            line.caret_count;
        const std::uint64_t selection_limit =
            static_cast<std::uint64_t>(
                line.first_selection_rect) +
            line.selection_rect_count;
        if (fragment_limit >
                request.projection->fragment_rects.size() ||
            caret_limit > request.projection->carets.size() ||
            selection_limit >
                request.projection->selection_rects.size()) {
            return fail(
                TextPaintCommandStreamErrorKind::TopologyViolation,
                index,
                line.first_fragment_rect,
                0U,
                0U,
                "projected line payload slice is outside its source array",
                error);
        }
    }
    return true;
}

bool within_limits(
    const PaintCounts& counts,
    const TextPaintCommandStreamLimits& limits) noexcept {
    return counts.commands <= limits.maximum_commands &&
           counts.glyph_batches <= limits.maximum_glyph_batches &&
           counts.fill_rects <= limits.maximum_fill_rects &&
           counts.referenced_glyphs <=
               limits.maximum_referenced_glyphs &&
           counts.commands <=
               std::numeric_limits<std::uint32_t>::max() &&
           counts.glyph_batches <=
               std::numeric_limits<std::uint32_t>::max() &&
           counts.fill_rects <=
               std::numeric_limits<std::uint32_t>::max();
}

} // namespace

TextPaintCommandStream::TextPaintCommandStream(
    std::pmr::memory_resource* resource)
    : clips(resource),
      commands(resource),
      glyph_batches(resource),
      fill_rects(resource) {}

std::pmr::memory_resource*
TextPaintCommandStream::resource() const noexcept {
    return commands.get_allocator().resource();
}

void TextPaintCommandStream::release() noexcept {
    release_vector(&clips);
    release_vector(&commands);
    release_vector(&glyph_batches);
    release_vector(&fill_rects);
}

const char* text_paint_command_stream_error_kind_name(
    TextPaintCommandStreamErrorKind kind) noexcept {
    switch (kind) {
    case TextPaintCommandStreamErrorKind::None:
        return "none";
    case TextPaintCommandStreamErrorKind::InvalidInput:
        return "invalid-input";
    case TextPaintCommandStreamErrorKind::TopologyViolation:
        return "topology-violation";
    case TextPaintCommandStreamErrorKind::MissingGlyphSpan:
        return "missing-glyph-span";
    case TextPaintCommandStreamErrorKind::AdvanceMismatch:
        return "advance-mismatch";
    case TextPaintCommandStreamErrorKind::CaretNotFound:
        return "caret-not-found";
    case TextPaintCommandStreamErrorKind::ArithmeticOverflow:
        return "arithmetic-overflow";
    case TextPaintCommandStreamErrorKind::CommandLimitExceeded:
        return "command-limit-exceeded";
    case TextPaintCommandStreamErrorKind::OutputBudgetExceeded:
        return "output-budget-exceeded";
    case TextPaintCommandStreamErrorKind::AggregateOverflow:
        return "aggregate-overflow";
    }
    return "unknown";
}

bool build_text_paint_command_stream(
    const TextPaintCommandStreamRequest& request,
    TextPaintCommandStream* output,
    TextPaintCommandStreamStats* stats,
    TextPaintCommandStreamError* error) noexcept {
    clear_error(error);
    if (stats != nullptr) {
        *stats = {};
    }
    if (output == nullptr) {
        return fail(
            TextPaintCommandStreamErrorKind::InvalidInput,
            0U,
            0U,
            0U,
            0U,
            "output command stream is null",
            error);
    }
    output->release();

    if (request.projection == nullptr ||
        request.fragment_layout == nullptr ||
        request.shaped_text == nullptr ||
        request.cluster_map == nullptr ||
        request.clip_inline_size == 0U ||
        request.clip_block_size == 0U ||
        request.clip_inline_size >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()) ||
        request.clip_block_size >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()) ||
        (!request.segment_style_ids.empty() &&
         request.segment_style_ids.size() !=
             request.shaped_text->segments.size())) {
        return fail(
            TextPaintCommandStreamErrorKind::InvalidInput,
            0U,
            0U,
            0U,
            0U,
            "paint request pointers, clip, or style table are invalid",
            error);
    }
    if (!validate_projection_topology(request, error)) {
        return false;
    }

    TextPaintCommandStreamStats analysis_stats;
    analysis_stats.input_lines =
        request.projection->lines.size();
    analysis_stats.input_fragment_rects =
        request.projection->fragment_rects.size();
    analysis_stats.input_selection_rects =
        request.projection->selection_rects.size();
    analysis_stats.input_carets =
        request.projection->carets.size();
    for (const ViewportLineRecord& line :
         request.projection->lines) {
        if ((line.flags & kViewportLineBeforeViewport) != 0U) {
            ++analysis_stats.lines_before_viewport;
        }
        if ((line.flags & kViewportLineAfterViewport) != 0U) {
            ++analysis_stats.lines_after_viewport;
        }
    }

    PaintCounts counts;
    if (!process_selection_rects(
            false,
            request,
            output,
            &counts,
            &analysis_stats,
            error) ||
        !process_glyph_batches(
            false,
            request,
            output,
            &counts,
            &analysis_stats,
            error) ||
        !process_caret(
            false,
            request,
            output,
            &counts,
            &analysis_stats,
            error)) {
        return false;
    }
    if (!within_limits(counts, request.limits)) {
        return fail(
            TextPaintCommandStreamErrorKind::CommandLimitExceeded,
            0U,
            0U,
            0U,
            0U,
            "paint command or referenced-glyph hard limit exceeded",
            error);
    }

    try {
        output->clips.reserve(1U);
        output->commands.reserve(
            static_cast<std::size_t>(counts.commands));
        output->glyph_batches.reserve(
            static_cast<std::size_t>(counts.glyph_batches));
        output->fill_rects.reserve(
            static_cast<std::size_t>(counts.fill_rects));
        output->clips.push_back({
            0,
            0,
            request.clip_inline_size,
            request.clip_block_size});

        PaintCounts emitted;
        if (!process_selection_rects(
                true,
                request,
                output,
                &emitted,
                nullptr,
                error) ||
            !process_glyph_batches(
                true,
                request,
                output,
                &emitted,
                nullptr,
                error) ||
            !process_caret(
                true,
                request,
                output,
                &emitted,
                nullptr,
                error)) {
            output->release();
            return false;
        }
        if (emitted.selection_rects != counts.selection_rects ||
            emitted.glyph_batches != counts.glyph_batches ||
            emitted.caret_rects != counts.caret_rects ||
            emitted.commands != counts.commands ||
            emitted.fill_rects != counts.fill_rects ||
            emitted.referenced_glyphs != counts.referenced_glyphs ||
            output->commands.size() != counts.commands ||
            output->glyph_batches.size() != counts.glyph_batches ||
            output->fill_rects.size() != counts.fill_rects) {
            output->release();
            return fail(
                TextPaintCommandStreamErrorKind::TopologyViolation,
                0U,
                0U,
                0U,
                0U,
                "paint analysis and publication passes diverged",
                error);
        }
    } catch (const std::bad_alloc&) {
        output->release();
        return fail(
            TextPaintCommandStreamErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            0U,
            "paint command output memory budget exceeded",
            error);
    } catch (...) {
        output->release();
        return fail(
            TextPaintCommandStreamErrorKind::InvalidInput,
            0U,
            0U,
            0U,
            0U,
            "paint command stream construction failed",
            error);
    }

    analysis_stats.output_clips = output->clips.size();
    analysis_stats.output_commands = output->commands.size();
    analysis_stats.output_glyph_batches =
        output->glyph_batches.size();
    analysis_stats.output_fill_rects = output->fill_rects.size();
    analysis_stats.selection_commands =
        counts.selection_rects;
    analysis_stats.caret_commands = counts.caret_rects;
    analysis_stats.referenced_glyphs =
        counts.referenced_glyphs;
    if (stats != nullptr) {
        *stats = analysis_stats;
    }
    return true;
}

} // namespace zevryon::text
