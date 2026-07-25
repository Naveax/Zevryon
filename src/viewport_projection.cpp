#include "viewport_projection.hpp"

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

void clear_error(ViewportProjectionError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
}

bool fail(
    ViewportProjectionErrorKind kind,
    std::size_t line_index,
    std::size_t fragment_index,
    std::uint32_t cluster_index,
    const char* message,
    ViewportProjectionError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->line_index = line_index;
        error->fragment_index = fragment_index;
        error->cluster_index = cluster_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool checked_add(
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

bool checked_add_i64(
    std::int64_t left,
    std::int32_t right,
    std::int64_t* output) noexcept {
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

bool checked_size_add(
    std::size_t left,
    std::size_t right,
    std::size_t* output) noexcept {
    if (output == nullptr ||
        right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    *output = left + right;
    return true;
}

bool signed_delta(
    std::uint64_t value,
    std::uint64_t origin,
    std::int64_t* output) noexcept {
    if (output == nullptr) {
        return false;
    }
    if (value >= origin) {
        const std::uint64_t delta = value - origin;
        if (delta > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        *output = static_cast<std::int64_t>(delta);
        return true;
    }
    const std::uint64_t delta = origin - value;
    if (delta > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    *output = -static_cast<std::int64_t>(delta);
    return true;
}

std::uint64_t absolute_difference(
    std::int64_t left,
    std::int64_t right) noexcept {
    if (left >= right) {
        return static_cast<std::uint64_t>(left) -
               static_cast<std::uint64_t>(right);
    }
    return static_cast<std::uint64_t>(right) -
           static_cast<std::uint64_t>(left);
}

bool absolute_value(std::int64_t value, std::uint64_t* output) noexcept {
    if (output == nullptr) {
        return false;
    }
    if (value >= 0) {
        *output = static_cast<std::uint64_t>(value);
        return true;
    }
    *output = static_cast<std::uint64_t>(-(value + 1)) + 1U;
    return true;
}

bool safe_boundary(
    const CaretBoundaryMap& map,
    std::uint32_t boundary) noexcept {
    return boundary < map.flags.size() &&
           (map.flags[boundary] &
            static_cast<std::uint8_t>(kCaretBoundarySafe)) != 0U;
}

bool boundary_has(
    const CaretBoundaryMap& map,
    std::uint32_t boundary,
    CaretBoundaryFlags flag) noexcept {
    return boundary < map.flags.size() &&
           (map.flags[boundary] & static_cast<std::uint8_t>(flag)) != 0U;
}

struct ProjectionCounts final {
    std::size_t lines{0};
    std::size_t fragments{0};
    std::size_t carets{0};
    std::size_t selections{0};
};

struct FragmentWalkResult final {
    std::size_t caret_count{0};
    std::size_t selection_count{0};
    std::uint64_t glyph_groups{0};
    std::uint64_t unsafe_skipped{0};
};

struct BuildContext final {
    const ViewportProjectionRequest* request{nullptr};
    std::uint64_t viewport_inline_end{0};
    std::uint64_t viewport_block_end{0};
    std::uint64_t inline_window_start{0};
    std::uint64_t inline_window_end{0};
    std::uint64_t block_window_start{0};
    std::uint64_t block_window_end{0};
    std::uint32_t cluster_count{0};
};

bool group_advance(
    const MultiRunShapedSegment& segment,
    const GlyphClusterRecord& record,
    std::uint64_t* output) noexcept {
    if (output == nullptr ||
        record.first_glyph > segment.glyphs.glyphs.size() ||
        record.glyph_count >
            segment.glyphs.glyphs.size() - record.first_glyph) {
        return false;
    }
    std::int64_t signed_advance = 0;
    for (std::uint32_t index = 0U; index < record.glyph_count; ++index) {
        const ShapedGlyph& glyph =
            segment.glyphs.glyphs[record.first_glyph + index];
        if (!checked_add_i64(
                signed_advance,
                glyph.x_advance,
                &signed_advance)) {
            return false;
        }
    }
    return absolute_value(signed_advance, output);
}

bool same_group_record(
    const GlyphClusterRecord& left,
    const GlyphClusterRecord& right) noexcept {
    return left.segment_index == right.segment_index &&
           left.owner_cluster == right.owner_cluster &&
           left.first_glyph == right.first_glyph &&
           left.glyph_count == right.glyph_count;
}

bool append_caret(
    bool emit,
    std::uint32_t boundary,
    std::uint64_t document_inline_position,
    std::uint64_t document_block_start,
    std::uint64_t block_size,
    std::uint32_t source_fragment_index,
    std::uint32_t line_first_cluster,
    std::uint32_t line_cluster_limit,
    std::uint32_t fragment_first_cluster,
    std::uint32_t fragment_cluster_limit,
    bool rtl,
    const BuildContext& context,
    std::pmr::vector<ViewportCaretEdge>* carets,
    FragmentWalkResult* result,
    ViewportProjectionError* error,
    std::size_t line_index) noexcept {
    const CaretBoundaryMap& map = *context.request->caret_boundaries;
    if (!safe_boundary(map, boundary)) {
        ++result->unsafe_skipped;
        return true;
    }
    std::int64_t inline_relative = 0;
    std::int64_t block_relative = 0;
    if (!signed_delta(
            document_inline_position,
            context.request->viewport_inline_start,
            &inline_relative) ||
        !signed_delta(
            document_block_start,
            context.request->viewport_block_start,
            &block_relative)) {
        return fail(
            ViewportProjectionErrorKind::ArithmeticOverflow,
            line_index,
            source_fragment_index,
            boundary,
            "viewport-relative caret coordinate exceeds the signed contract",
            error);
    }
    std::uint32_t flags = rtl ? kViewportCaretRtl : 0U;
    if (boundary == line_first_cluster ||
        boundary == line_cluster_limit) {
        flags |= kViewportCaretLineEdge;
    }
    if (boundary == fragment_first_cluster ||
        boundary == fragment_cluster_limit) {
        flags |= kViewportCaretFragmentEdge;
    }
    if (boundary_has(map, boundary, kCaretBoundaryTextEdge)) {
        flags |= kViewportCaretTextEdge;
    }
    if (emit) {
        carets->push_back(ViewportCaretEdge{
            inline_relative,
            block_relative,
            block_size,
            boundary,
            source_fragment_index,
            flags,
            0U});
    }
    ++result->caret_count;
    return true;
}

bool projected_inline(
    std::uint64_t start,
    std::uint64_t size,
    const BuildContext& context,
    std::uint64_t* end) noexcept {
    if (!checked_add(start, size, end)) {
        return false;
    }
    if (size == 0U) {
        return start >= context.inline_window_start &&
               start <= context.inline_window_end;
    }
    return start < context.inline_window_end &&
           *end > context.inline_window_start;
}

bool walk_fragment(
    bool emit,
    const BuildContext& context,
    std::size_t source_line_index,
    std::uint32_t line_first_cluster,
    std::uint32_t line_cluster_limit,
    std::size_t source_fragment_index,
    const InlineLayoutFragment& fragment,
    const FragmentBlockMetric& block_metric,
    const LineBoxRecord& line_box,
    std::pmr::vector<ViewportCaretEdge>* carets,
    std::pmr::vector<ViewportSelectionRect>* selections,
    FragmentWalkResult* result,
    ViewportProjectionError* error) noexcept {
    *result = {};
    const auto& request = *context.request;
    const bool rtl =
        (fragment.flags & kInlineFragmentGlyphRunRtl) != 0U;
    if (fragment.segment_index >= request.shaped_text->segments.size() ||
        fragment.first_cluster >= fragment.cluster_limit ||
        fragment.cluster_limit > context.cluster_count ||
        fragment.first_cluster < line_first_cluster ||
        fragment.cluster_limit > line_cluster_limit) {
        return fail(
            ViewportProjectionErrorKind::TopologyViolation,
            source_line_index,
            source_fragment_index,
            fragment.first_cluster,
            "fragment range or shaped segment is outside the certified line domain",
            error);
    }
    const MultiRunShapedSegment& segment =
        request.shaped_text->segments[fragment.segment_index];
    if ((rtl &&
         segment.glyphs.direction != ShapingDirection::RightToLeft) ||
        (!rtl &&
         segment.glyphs.direction != ShapingDirection::LeftToRight)) {
        return fail(
            ViewportProjectionErrorKind::TopologyViolation,
            source_line_index,
            source_fragment_index,
            fragment.first_cluster,
            "fragment direction does not match the retained shaped segment",
            error);
    }

    std::uint64_t fragment_block_start = 0U;
    if (!checked_add(
            line_box.block_start,
            block_metric.block_offset,
            &fragment_block_start)) {
        return fail(
            ViewportProjectionErrorKind::ArithmeticOverflow,
            source_line_index,
            source_fragment_index,
            fragment.first_cluster,
            "fragment block coordinate overflow",
            error);
    }

    const bool has_selection = request.selection.enabled &&
        request.selection.first_boundary <
            request.selection.boundary_limit;
    const std::uint32_t selection_first = has_selection
        ? std::max(
              request.selection.first_boundary,
              fragment.first_cluster)
        : 0U;
    const std::uint32_t selection_limit = has_selection
        ? std::min(
              request.selection.boundary_limit,
              fragment.cluster_limit)
        : 0U;
    const bool fragment_selected =
        has_selection && selection_first < selection_limit;
    bool found_selection_first = false;
    bool found_selection_limit = false;
    std::uint64_t selection_first_position = 0U;
    std::uint64_t selection_limit_position = 0U;

    auto observe_boundary = [
        &](std::uint32_t boundary, std::uint64_t position) noexcept {
        if (fragment_selected && boundary == selection_first) {
            found_selection_first = true;
            selection_first_position = position;
        }
        if (fragment_selected && boundary == selection_limit) {
            found_selection_limit = true;
            selection_limit_position = position;
        }
    };

    std::uint64_t cursor_position = fragment.inline_offset;
    std::uint64_t accumulated_advance = 0U;
    if (!rtl) {
        observe_boundary(fragment.first_cluster, cursor_position);
        if (!append_caret(
                emit,
                fragment.first_cluster,
                cursor_position,
                fragment_block_start,
                block_metric.block_size,
                static_cast<std::uint32_t>(source_fragment_index),
                line_first_cluster,
                line_cluster_limit,
                fragment.first_cluster,
                fragment.cluster_limit,
                false,
                context,
                carets,
                result,
                error,
                source_line_index)) {
            return false;
        }
        std::uint32_t cluster = fragment.first_cluster;
        while (cluster < fragment.cluster_limit) {
            const GlyphClusterRecord& owner =
                request.cluster_map->records[cluster];
            if (owner.owner_cluster != cluster ||
                owner.segment_index != fragment.segment_index) {
                return fail(
                    ViewportProjectionErrorKind::TopologyViolation,
                    source_line_index,
                    source_fragment_index,
                    cluster,
                    "LTR fragment does not start each glyph group at its owner cluster",
                    error);
            }
            std::uint32_t group_limit = cluster + 1U;
            while (group_limit < fragment.cluster_limit &&
                   request.cluster_map->records[group_limit]
                           .owner_cluster == cluster) {
                if (!same_group_record(
                        owner,
                        request.cluster_map->records[group_limit])) {
                    return fail(
                        ViewportProjectionErrorKind::TopologyViolation,
                        source_line_index,
                        source_fragment_index,
                        group_limit,
                        "continuation cluster does not preserve its owner glyph span",
                        error);
                }
                ++group_limit;
            }
            std::uint64_t advance = 0U;
            if (!group_advance(segment, owner, &advance) ||
                !checked_add(
                    cursor_position,
                    advance,
                    &cursor_position) ||
                !checked_add(
                    accumulated_advance,
                    advance,
                    &accumulated_advance)) {
                return fail(
                    ViewportProjectionErrorKind::ArithmeticOverflow,
                    source_line_index,
                    source_fragment_index,
                    cluster,
                    "glyph-group advance exceeds the projection contract",
                    error);
            }
            ++result->glyph_groups;
            observe_boundary(group_limit, cursor_position);
            if (!append_caret(
                    emit,
                    group_limit,
                    cursor_position,
                    fragment_block_start,
                    block_metric.block_size,
                    static_cast<std::uint32_t>(source_fragment_index),
                    line_first_cluster,
                    line_cluster_limit,
                    fragment.first_cluster,
                    fragment.cluster_limit,
                    false,
                    context,
                    carets,
                    result,
                    error,
                    source_line_index)) {
                return false;
            }
            cluster = group_limit;
        }
    } else {
        observe_boundary(fragment.cluster_limit, cursor_position);
        if (!append_caret(
                emit,
                fragment.cluster_limit,
                cursor_position,
                fragment_block_start,
                block_metric.block_size,
                static_cast<std::uint32_t>(source_fragment_index),
                line_first_cluster,
                line_cluster_limit,
                fragment.first_cluster,
                fragment.cluster_limit,
                true,
                context,
                carets,
                result,
                error,
                source_line_index)) {
            return false;
        }
        std::uint32_t cluster_limit = fragment.cluster_limit;
        while (cluster_limit > fragment.first_cluster) {
            const GlyphClusterRecord& tail =
                request.cluster_map->records[cluster_limit - 1U];
            const std::uint32_t owner_cluster = tail.owner_cluster;
            if (owner_cluster < fragment.first_cluster ||
                owner_cluster >= cluster_limit ||
                tail.segment_index != fragment.segment_index) {
                return fail(
                    ViewportProjectionErrorKind::TopologyViolation,
                    source_line_index,
                    source_fragment_index,
                    cluster_limit - 1U,
                    "RTL fragment carries an invalid owner cluster",
                    error);
            }
            const GlyphClusterRecord& owner =
                request.cluster_map->records[owner_cluster];
            if (owner.owner_cluster != owner_cluster ||
                owner.segment_index != fragment.segment_index) {
                return fail(
                    ViewportProjectionErrorKind::TopologyViolation,
                    source_line_index,
                    source_fragment_index,
                    owner_cluster,
                    "RTL glyph group owner is inconsistent",
                    error);
            }
            for (std::uint32_t index = owner_cluster;
                 index < cluster_limit;
                 ++index) {
                if (!same_group_record(
                        owner,
                        request.cluster_map->records[index])) {
                    return fail(
                        ViewportProjectionErrorKind::TopologyViolation,
                        source_line_index,
                        source_fragment_index,
                        index,
                        "RTL continuation cluster does not preserve its owner glyph span",
                        error);
                }
            }
            std::uint64_t advance = 0U;
            if (!group_advance(segment, owner, &advance) ||
                !checked_add(
                    cursor_position,
                    advance,
                    &cursor_position) ||
                !checked_add(
                    accumulated_advance,
                    advance,
                    &accumulated_advance)) {
                return fail(
                    ViewportProjectionErrorKind::ArithmeticOverflow,
                    source_line_index,
                    source_fragment_index,
                    owner_cluster,
                    "RTL glyph-group advance exceeds the projection contract",
                    error);
            }
            ++result->glyph_groups;
            observe_boundary(owner_cluster, cursor_position);
            if (!append_caret(
                    emit,
                    owner_cluster,
                    cursor_position,
                    fragment_block_start,
                    block_metric.block_size,
                    static_cast<std::uint32_t>(source_fragment_index),
                    line_first_cluster,
                    line_cluster_limit,
                    fragment.first_cluster,
                    fragment.cluster_limit,
                    true,
                    context,
                    carets,
                    result,
                    error,
                    source_line_index)) {
                return false;
            }
            cluster_limit = owner_cluster;
        }
    }

    if (accumulated_advance != fragment.inline_advance) {
        return fail(
            ViewportProjectionErrorKind::TopologyViolation,
            source_line_index,
            source_fragment_index,
            fragment.first_cluster,
            "glyph-group advances do not reproduce the visual fragment advance",
            error);
    }

    if (fragment_selected) {
        if (!found_selection_first || !found_selection_limit) {
            return fail(
                ViewportProjectionErrorKind::TopologyViolation,
                source_line_index,
                source_fragment_index,
                selection_first,
                "selection boundary is not a retained glyph-group edge",
                error);
        }
        const std::uint64_t selection_start = std::min(
            selection_first_position,
            selection_limit_position);
        const std::uint64_t selection_end = std::max(
            selection_first_position,
            selection_limit_position);
        if (selection_start != selection_end) {
            std::int64_t inline_relative = 0;
            std::int64_t block_relative = 0;
            if (!signed_delta(
                    selection_start,
                    request.viewport_inline_start,
                    &inline_relative) ||
                !signed_delta(
                    line_box.block_start,
                    request.viewport_block_start,
                    &block_relative)) {
                return fail(
                    ViewportProjectionErrorKind::ArithmeticOverflow,
                    source_line_index,
                    source_fragment_index,
                    selection_first,
                    "selection rectangle exceeds the signed viewport coordinate contract",
                    error);
            }
            std::uint32_t flags =
                rtl ? kViewportSelectionRtl : 0U;
            if (request.selection.first_boundary >
                fragment.first_cluster) {
                flags |= kViewportSelectionStartsInsideFragment;
            }
            if (request.selection.boundary_limit <
                fragment.cluster_limit) {
                flags |= kViewportSelectionEndsInsideFragment;
            }
            if (emit) {
                selections->push_back(ViewportSelectionRect{
                    inline_relative,
                    block_relative,
                    selection_end - selection_start,
                    line_box.block_size,
                    static_cast<std::uint32_t>(source_line_index),
                    static_cast<std::uint32_t>(source_fragment_index),
                    flags,
                    0U});
            }
            result->selection_count = 1U;
        }
    }
    return true;
}

bool validate_and_prepare(
    const ViewportProjectionRequest& request,
    BuildContext* context,
    std::uint32_t* first_line,
    std::uint32_t* line_limit,
    ViewportProjectionError* error) noexcept {
    if (context == nullptr || first_line == nullptr ||
        line_limit == nullptr || request.line_boxes == nullptr ||
        request.fragment_layout == nullptr ||
        request.shaped_text == nullptr ||
        request.cluster_map == nullptr ||
        request.caret_boundaries == nullptr ||
        request.viewport_inline_size == 0U ||
        request.viewport_block_size == 0U ||
        request.limits.maximum_lines == 0U ||
        request.limits.maximum_fragment_rects == 0U ||
        request.limits.maximum_carets == 0U ||
        request.limits.maximum_selection_rects == 0U) {
        return fail(
            ViewportProjectionErrorKind::InvalidInput,
            0U,
            0U,
            0U,
            "projection requires complete inputs, non-empty viewport dimensions and non-zero output limits",
            error);
    }
    const LineBoxLayout& boxes = *request.line_boxes;
    const LineFragmentLayout& fragments = *request.fragment_layout;
    if (boxes.lines.size() != fragments.lines.size() ||
        boxes.fragment_metrics.size() != fragments.fragments.size() ||
        boxes.lines.size() > static_cast<std::size_t>(
                                 std::numeric_limits<std::uint32_t>::max()) ||
        fragments.fragments.size() > static_cast<std::size_t>(
                                         std::numeric_limits<std::uint32_t>::max()) ||
        request.cluster_map->records.size() + 1U !=
            request.caret_boundaries->flags.size() ||
        request.cluster_map->records.size() > static_cast<std::size_t>(
                                                  std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            ViewportProjectionErrorKind::TopologyViolation,
            0U,
            0U,
            0U,
            "line, fragment, cluster and caret arrays do not share one bounded domain",
            error);
    }
    context->request = &request;
    context->cluster_count = static_cast<std::uint32_t>(
        request.cluster_map->records.size());
    if (!checked_add(
            request.viewport_inline_start,
            request.viewport_inline_size,
            &context->viewport_inline_end) ||
        !checked_add(
            request.viewport_block_start,
            request.viewport_block_size,
            &context->viewport_block_end) ||
        !checked_add(
            context->viewport_inline_end,
            request.inline_overscan,
            &context->inline_window_end) ||
        !checked_add(
            context->viewport_block_end,
            request.block_overscan,
            &context->block_window_end)) {
        return fail(
            ViewportProjectionErrorKind::ArithmeticOverflow,
            0U,
            0U,
            0U,
            "viewport or overscan range overflows the 64-bit coordinate contract",
            error);
    }
    context->inline_window_start =
        request.viewport_inline_start > request.inline_overscan
            ? request.viewport_inline_start - request.inline_overscan
            : 0U;
    context->block_window_start =
        request.viewport_block_start > request.block_overscan
            ? request.viewport_block_start - request.block_overscan
            : 0U;

    if (request.selection.enabled) {
        if (request.selection.first_boundary >
                request.selection.boundary_limit ||
            request.selection.boundary_limit > context->cluster_count ||
            !safe_boundary(
                *request.caret_boundaries,
                request.selection.first_boundary) ||
            !safe_boundary(
                *request.caret_boundaries,
                request.selection.boundary_limit)) {
            return fail(
                ViewportProjectionErrorKind::InvalidInput,
                0U,
                0U,
                request.selection.first_boundary,
                "selection endpoints must be ordered safe caret boundaries inside the cluster domain",
                error);
        }
    }

    std::uint64_t previous_end = 0U;
    std::uint32_t previous_cluster_limit = 0U;
    for (std::size_t line_index = 0U;
         line_index < boxes.lines.size();
         ++line_index) {
        const LineBoxRecord& box = boxes.lines[line_index];
        const VisualLineLayoutRecord& visual =
            fragments.lines[line_index];
        std::uint64_t line_end = 0U;
        if (!checked_add(box.block_start, box.block_size, &line_end) ||
            box.block_size == 0U || box.baseline < box.block_start ||
            box.baseline > line_end ||
            box.block_start != previous_end ||
            box.first_fragment_metric != visual.first_fragment ||
            box.fragment_metric_count != visual.fragment_count ||
            box.inline_advance != visual.inline_advance ||
            box.cluster_limit != visual.cluster_limit ||
            box.cluster_limit < previous_cluster_limit ||
            static_cast<std::size_t>(visual.first_fragment) >
                fragments.fragments.size() ||
            static_cast<std::size_t>(visual.fragment_count) >
                fragments.fragments.size() - visual.first_fragment) {
            return fail(
                ViewportProjectionErrorKind::TopologyViolation,
                line_index,
                visual.first_fragment,
                previous_cluster_limit,
                "line-box and visual-fragment topology is inconsistent",
                error);
        }
        std::uint64_t inline_cursor = 0U;
        std::uint64_t covered_clusters = 0U;
        for (std::uint32_t local = 0U;
             local < visual.fragment_count;
             ++local) {
            const std::size_t fragment_index =
                visual.first_fragment + local;
            const InlineLayoutFragment& fragment =
                fragments.fragments[fragment_index];
            const FragmentBlockMetric& metric =
                boxes.fragment_metrics[fragment_index];
            std::uint64_t fragment_end = 0U;
            std::uint64_t metric_end = 0U;
            if (fragment.inline_offset != inline_cursor ||
                !checked_add(
                    fragment.inline_offset,
                    fragment.inline_advance,
                    &fragment_end) ||
                fragment.segment_index >=
                    request.shaped_text->segments.size() ||
                fragment.first_cluster >= fragment.cluster_limit ||
                fragment.first_cluster < previous_cluster_limit ||
                fragment.cluster_limit > box.cluster_limit ||
                !checked_add(
                    metric.block_offset,
                    metric.block_size,
                    &metric_end) ||
                metric_end > box.block_size ||
                metric.baseline_offset > metric.block_size) {
                return fail(
                    ViewportProjectionErrorKind::TopologyViolation,
                    line_index,
                    fragment_index,
                    fragment.first_cluster,
                    "fragment geometry or block metrics are outside their source line",
                    error);
            }
            inline_cursor = fragment_end;
            covered_clusters +=
                fragment.cluster_limit - fragment.first_cluster;
        }
        if (inline_cursor != visual.inline_advance ||
            covered_clusters != static_cast<std::uint64_t>(
                                    box.cluster_limit -
                                    previous_cluster_limit)) {
            return fail(
                ViewportProjectionErrorKind::TopologyViolation,
                line_index,
                visual.first_fragment,
                previous_cluster_limit,
                "visual fragments do not exactly cover the source line domain",
                error);
        }
        previous_end = line_end;
        previous_cluster_limit = box.cluster_limit;
    }
    if (previous_cluster_limit != context->cluster_count) {
        return fail(
            ViewportProjectionErrorKind::TopologyViolation,
            boxes.lines.size(),
            fragments.fragments.size(),
            previous_cluster_limit,
            "line cluster limits do not cover the complete cluster map",
            error);
    }

    std::size_t low = 0U;
    std::size_t high = boxes.lines.size();
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2U;
        std::uint64_t line_end = 0U;
        if (!checked_add(
                boxes.lines[middle].block_start,
                boxes.lines[middle].block_size,
                &line_end)) {
            return fail(
                ViewportProjectionErrorKind::ArithmeticOverflow,
                middle,
                0U,
                0U,
                "line end overflows during viewport search",
                error);
        }
        if (line_end <= context->block_window_start) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    *first_line = static_cast<std::uint32_t>(low);
    low = *first_line;
    high = boxes.lines.size();
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2U;
        if (boxes.lines[middle].block_start <
            context->block_window_end) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    *line_limit = static_cast<std::uint32_t>(low);
    return true;
}

bool count_projection(
    const BuildContext& context,
    std::uint32_t first_line,
    std::uint32_t line_limit,
    ProjectionCounts* counts,
    ViewportProjectionStats* stats,
    ViewportProjectionError* error) noexcept {
    const auto& request = *context.request;
    const auto& boxes = *request.line_boxes;
    const auto& layout = *request.fragment_layout;
    counts->lines = line_limit - first_line;
    if (counts->lines > request.limits.maximum_lines) {
        return fail(
            ViewportProjectionErrorKind::ProjectionLimitExceeded,
            first_line,
            0U,
            0U,
            "visible line window exceeds the configured projection limit",
            error);
    }
    std::uint32_t line_first_cluster = first_line == 0U
        ? 0U
        : boxes.lines[first_line - 1U].cluster_limit;
    for (std::uint32_t line_index = first_line;
         line_index < line_limit;
         ++line_index) {
        const LineBoxRecord& box = boxes.lines[line_index];
        const VisualLineLayoutRecord& visual =
            layout.lines[line_index];
        std::size_t line_fragments = 0U;
        std::size_t line_carets = 0U;
        std::size_t line_selections = 0U;
        for (std::uint32_t local = 0U;
             local < visual.fragment_count;
             ++local) {
            const std::size_t fragment_index =
                visual.first_fragment + local;
            const InlineLayoutFragment& fragment =
                layout.fragments[fragment_index];
            std::uint64_t fragment_end = 0U;
            const bool include = projected_inline(
                fragment.inline_offset,
                fragment.inline_advance,
                context,
                &fragment_end);
            if (!include) {
                continue;
            }
            FragmentWalkResult walked;
            if (!walk_fragment(
                    false,
                    context,
                    line_index,
                    line_first_cluster,
                    box.cluster_limit,
                    fragment_index,
                    fragment,
                    boxes.fragment_metrics[fragment_index],
                    box,
                    nullptr,
                    nullptr,
                    &walked,
                    error)) {
                return false;
            }
            if (!checked_size_add(
                    counts->fragments,
                    1U,
                    &counts->fragments) ||
                !checked_size_add(
                    counts->carets,
                    walked.caret_count,
                    &counts->carets) ||
                !checked_size_add(
                    counts->selections,
                    walked.selection_count,
                    &counts->selections)) {
                return fail(
                    ViewportProjectionErrorKind::AggregateOverflow,
                    line_index,
                    fragment_index,
                    fragment.first_cluster,
                    "projection record counts overflow size_t",
                    error);
            }
            ++line_fragments;
            line_carets += walked.caret_count;
            line_selections += walked.selection_count;
            stats->glyph_groups += walked.glyph_groups;
            stats->unsafe_caret_boundaries_skipped +=
                walked.unsafe_skipped;
        }
        stats->maximum_fragments_per_line =
            std::max<std::uint64_t>(
                stats->maximum_fragments_per_line,
                line_fragments);
        stats->maximum_carets_per_line =
            std::max<std::uint64_t>(
                stats->maximum_carets_per_line,
                line_carets);
        stats->maximum_selection_rects_per_line =
            std::max<std::uint64_t>(
                stats->maximum_selection_rects_per_line,
                line_selections);
        line_first_cluster = box.cluster_limit;
    }
    if (counts->fragments >
            request.limits.maximum_fragment_rects ||
        counts->carets > request.limits.maximum_carets ||
        counts->selections >
            request.limits.maximum_selection_rects) {
        return fail(
            ViewportProjectionErrorKind::ProjectionLimitExceeded,
            first_line,
            0U,
            0U,
            "projected fragment, caret or selection records exceed configured limits",
            error);
    }
    return true;
}

} // namespace

ViewportProjection::ViewportProjection(
    std::pmr::memory_resource* resource)
    : lines(resource),
      fragment_rects(resource),
      carets(resource),
      selection_rects(resource) {}

std::pmr::memory_resource* ViewportProjection::resource() const noexcept {
    return lines.get_allocator().resource();
}

void ViewportProjection::release() noexcept {
    release_vector(&lines);
    release_vector(&fragment_rects);
    release_vector(&carets);
    release_vector(&selection_rects);
    viewport_inline_start = 0U;
    viewport_block_start = 0U;
    document_block_extent = 0U;
}

const char* viewport_projection_error_kind_name(
    ViewportProjectionErrorKind kind) noexcept {
    switch (kind) {
        case ViewportProjectionErrorKind::None:
            return "none";
        case ViewportProjectionErrorKind::InvalidInput:
            return "invalid_input";
        case ViewportProjectionErrorKind::TopologyViolation:
            return "topology_violation";
        case ViewportProjectionErrorKind::ArithmeticOverflow:
            return "arithmetic_overflow";
        case ViewportProjectionErrorKind::ProjectionLimitExceeded:
            return "projection_limit_exceeded";
        case ViewportProjectionErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
        case ViewportProjectionErrorKind::AggregateOverflow:
            return "aggregate_overflow";
    }
    return "invalid";
}

bool build_viewport_projection(
    const ViewportProjectionRequest& request,
    ViewportProjection* output,
    ViewportProjectionStats* stats,
    ViewportProjectionError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    output->release();
    *stats = {};
    clear_error(error);

    BuildContext context;
    std::uint32_t first_line = 0U;
    std::uint32_t line_limit = 0U;
    if (!validate_and_prepare(
            request,
            &context,
            &first_line,
            &line_limit,
            error)) {
        return false;
    }

    stats->input_lines = request.line_boxes->lines.size();
    stats->input_fragments =
        request.fragment_layout->fragments.size();
    stats->input_clusters = context.cluster_count;
    stats->first_source_line = first_line;
    stats->source_line_limit = line_limit;

    ProjectionCounts counts;
    if (!count_projection(
            context,
            first_line,
            line_limit,
            &counts,
            stats,
            error)) {
        return false;
    }

    try {
        ViewportProjection working(output->resource());
        working.viewport_inline_start = request.viewport_inline_start;
        working.viewport_block_start = request.viewport_block_start;
        if (!request.line_boxes->lines.empty()) {
            const LineBoxRecord& last =
                request.line_boxes->lines.back();
            if (!checked_add(
                    last.block_start,
                    last.block_size,
                    &working.document_block_extent)) {
                return fail(
                    ViewportProjectionErrorKind::ArithmeticOverflow,
                    request.line_boxes->lines.size() - 1U,
                    0U,
                    0U,
                    "document block extent overflow",
                    error);
            }
        }
        working.lines.reserve(counts.lines);
        working.fragment_rects.reserve(counts.fragments);
        working.carets.reserve(counts.carets);
        working.selection_rects.reserve(counts.selections);

        std::uint32_t line_first_cluster = first_line == 0U
            ? 0U
            : request.line_boxes->lines[first_line - 1U]
                  .cluster_limit;
        for (std::uint32_t line_index = first_line;
             line_index < line_limit;
             ++line_index) {
            const LineBoxRecord& box =
                request.line_boxes->lines[line_index];
            const VisualLineLayoutRecord& visual =
                request.fragment_layout->lines[line_index];
            std::int64_t block_relative = 0;
            std::int64_t baseline_relative = 0;
            if (!signed_delta(
                    box.block_start,
                    request.viewport_block_start,
                    &block_relative) ||
                !signed_delta(
                    box.baseline,
                    request.viewport_block_start,
                    &baseline_relative)) {
                return fail(
                    ViewportProjectionErrorKind::ArithmeticOverflow,
                    line_index,
                    visual.first_fragment,
                    line_first_cluster,
                    "line coordinate exceeds the signed viewport contract",
                    error);
            }
            const std::size_t first_fragment_rect =
                working.fragment_rects.size();
            const std::size_t first_caret = working.carets.size();
            const std::size_t first_selection =
                working.selection_rects.size();
            std::uint32_t line_flags = 0U;
            std::uint64_t line_end = 0U;
            if (!checked_add(
                    box.block_start,
                    box.block_size,
                    &line_end)) {
                return fail(
                    ViewportProjectionErrorKind::ArithmeticOverflow,
                    line_index,
                    visual.first_fragment,
                    line_first_cluster,
                    "line end overflow during emission",
                    error);
            }
            if (line_end <= request.viewport_block_start) {
                line_flags |= kViewportLineBeforeViewport;
                ++stats->lines_before_viewport;
            }
            if (box.block_start >= context.viewport_block_end) {
                line_flags |= kViewportLineAfterViewport;
                ++stats->lines_after_viewport;
            }
            if ((visual.flags & kVisualLineContainsRtl) != 0U) {
                line_flags |= kViewportLineContainsRtl;
            }

            for (std::uint32_t local = 0U;
                 local < visual.fragment_count;
                 ++local) {
                const std::size_t fragment_index =
                    visual.first_fragment + local;
                const InlineLayoutFragment& fragment =
                    request.fragment_layout->fragments[fragment_index];
                std::uint64_t fragment_end = 0U;
                if (!projected_inline(
                        fragment.inline_offset,
                        fragment.inline_advance,
                        context,
                        &fragment_end)) {
                    continue;
                }
                const FragmentBlockMetric& metric =
                    request.line_boxes
                        ->fragment_metrics[fragment_index];
                std::uint64_t fragment_block_start = 0U;
                if (!checked_add(
                        box.block_start,
                        metric.block_offset,
                        &fragment_block_start)) {
                    return fail(
                        ViewportProjectionErrorKind::ArithmeticOverflow,
                        line_index,
                        fragment_index,
                        fragment.first_cluster,
                        "fragment block coordinate overflow during emission",
                        error);
                }
                std::int64_t inline_relative = 0;
                std::int64_t fragment_block_relative = 0;
                if (!signed_delta(
                        fragment.inline_offset,
                        request.viewport_inline_start,
                        &inline_relative) ||
                    !signed_delta(
                        fragment_block_start,
                        request.viewport_block_start,
                        &fragment_block_relative)) {
                    return fail(
                        ViewportProjectionErrorKind::ArithmeticOverflow,
                        line_index,
                        fragment_index,
                        fragment.first_cluster,
                        "fragment rectangle exceeds the signed viewport contract",
                        error);
                }
                std::uint32_t fragment_flags = 0U;
                if ((fragment.flags &
                     kInlineFragmentGlyphRunRtl) != 0U) {
                    fragment_flags |= kViewportFragmentRtl;
                    ++stats->rtl_fragment_rects;
                }
                if ((fragment.flags &
                     kInlineFragmentL1Adjusted) != 0U) {
                    fragment_flags |=
                        kViewportFragmentL1Adjusted;
                }
                if ((fragment.flags &
                     kInlineFragmentContainsX9Only) != 0U) {
                    fragment_flags |=
                        kViewportFragmentContainsX9Only;
                }
                if (fragment_end <= request.viewport_inline_start) {
                    fragment_flags |=
                        kViewportFragmentBeforeInlineViewport;
                }
                if (fragment.inline_offset >=
                    context.viewport_inline_end) {
                    fragment_flags |=
                        kViewportFragmentAfterInlineViewport;
                }
                working.fragment_rects.push_back(
                    ViewportFragmentRect{
                        inline_relative,
                        fragment_block_relative,
                        fragment.inline_advance,
                        metric.block_size,
                        static_cast<std::uint32_t>(fragment_index),
                        fragment.first_cluster,
                        fragment.cluster_limit,
                        fragment_flags});

                FragmentWalkResult walked;
                if (!walk_fragment(
                        true,
                        context,
                        line_index,
                        line_first_cluster,
                        box.cluster_limit,
                        fragment_index,
                        fragment,
                        metric,
                        box,
                        &working.carets,
                        &working.selection_rects,
                        &walked,
                        error)) {
                    return false;
                }
            }

            auto caret_begin = working.carets.begin() +
                static_cast<std::ptrdiff_t>(first_caret);
            std::sort(
                caret_begin,
                working.carets.end(),
                [](const ViewportCaretEdge& left,
                   const ViewportCaretEdge& right) {
                    if (left.viewport_inline_position !=
                        right.viewport_inline_position) {
                        return left.viewport_inline_position <
                               right.viewport_inline_position;
                    }
                    if (left.boundary_index !=
                        right.boundary_index) {
                        return left.boundary_index <
                               right.boundary_index;
                    }
                    return left.source_fragment_index <
                           right.source_fragment_index;
                });

            const std::size_t fragment_count =
                working.fragment_rects.size() - first_fragment_rect;
            const std::size_t caret_count =
                working.carets.size() - first_caret;
            const std::size_t selection_count =
                working.selection_rects.size() - first_selection;
            if (selection_count != 0U) {
                line_flags |= kViewportLineContainsSelection;
            }
            working.lines.push_back(ViewportLineRecord{
                block_relative,
                baseline_relative,
                box.block_size,
                box.inline_advance,
                line_index,
                static_cast<std::uint32_t>(first_fragment_rect),
                static_cast<std::uint32_t>(fragment_count),
                static_cast<std::uint32_t>(first_caret),
                static_cast<std::uint32_t>(caret_count),
                static_cast<std::uint32_t>(first_selection),
                static_cast<std::uint32_t>(selection_count),
                line_flags});
            line_first_cluster = box.cluster_limit;
        }

        if (working.lines.size() != counts.lines ||
            working.fragment_rects.size() != counts.fragments ||
            working.carets.size() != counts.carets ||
            working.selection_rects.size() != counts.selections) {
            return fail(
                ViewportProjectionErrorKind::TopologyViolation,
                first_line,
                0U,
                0U,
                "count and emission passes produced different projection sizes",
                error);
        }

        output->viewport_inline_start =
            working.viewport_inline_start;
        output->viewport_block_start =
            working.viewport_block_start;
        output->document_block_extent =
            working.document_block_extent;
        output->lines.swap(working.lines);
        output->fragment_rects.swap(working.fragment_rects);
        output->carets.swap(working.carets);
        output->selection_rects.swap(working.selection_rects);
        stats->output_lines = output->lines.size();
        stats->output_fragment_rects =
            output->fragment_rects.size();
        stats->output_carets = output->carets.size();
        stats->output_selection_rects =
            output->selection_rects.size();
        return true;
    } catch (const std::bad_alloc&) {
        output->release();
        return fail(
            ViewportProjectionErrorKind::OutputBudgetExceeded,
            first_line,
            0U,
            0U,
            "viewport projection exceeded its PMR budget",
            error);
    } catch (...) {
        output->release();
        return fail(
            ViewportProjectionErrorKind::InvalidInput,
            first_line,
            0U,
            0U,
            "viewport projection failed",
            error);
    }
}

bool hit_test_viewport_projection(
    const ViewportProjection& projection,
    std::int64_t viewport_inline_position,
    std::int64_t viewport_block_position,
    ViewportHitTestBias bias,
    ViewportHitTestResult* output) noexcept {
    if (output == nullptr || projection.lines.empty() ||
        projection.carets.empty() ||
        (bias != ViewportHitTestBias::Nearest &&
         bias != ViewportHitTestBias::TowardVisualStart &&
         bias != ViewportHitTestBias::TowardVisualEnd)) {
        return false;
    }
    *output = {};

    std::size_t selected_line = projection.lines.size();
    std::uint64_t best_block_distance =
        std::numeric_limits<std::uint64_t>::max();
    for (std::size_t index = 0U;
         index < projection.lines.size();
         ++index) {
        const ViewportLineRecord& line = projection.lines[index];
        if (line.caret_count == 0U) {
            continue;
        }
        if (line.block_size > static_cast<std::uint64_t>(
                                  std::numeric_limits<std::int64_t>::max()) ||
            line.viewport_block_start >
                std::numeric_limits<std::int64_t>::max() -
                    static_cast<std::int64_t>(line.block_size)) {
            return false;
        }
        const std::int64_t line_end =
            line.viewport_block_start +
            static_cast<std::int64_t>(line.block_size);
        std::uint64_t distance = 0U;
        if (viewport_block_position < line.viewport_block_start) {
            distance = absolute_difference(
                viewport_block_position,
                line.viewport_block_start);
        } else if (viewport_block_position > line_end) {
            distance = absolute_difference(
                viewport_block_position,
                line_end);
        }
        if (distance < best_block_distance) {
            best_block_distance = distance;
            selected_line = index;
        }
    }
    if (selected_line == projection.lines.size()) {
        return false;
    }

    const ViewportLineRecord& line =
        projection.lines[selected_line];
    const std::size_t first = line.first_caret;
    const std::size_t limit = first + line.caret_count;
    if (limit > projection.carets.size()) {
        return false;
    }
    const auto begin = projection.carets.begin() +
        static_cast<std::ptrdiff_t>(first);
    const auto end = projection.carets.begin() +
        static_cast<std::ptrdiff_t>(limit);
    const auto lower = std::lower_bound(
        begin,
        end,
        viewport_inline_position,
        [](const ViewportCaretEdge& caret, std::int64_t value) {
            return caret.viewport_inline_position < value;
        });

    const ViewportCaretEdge* chosen = nullptr;
    if (lower == begin) {
        chosen = &*lower;
    } else if (lower == end) {
        chosen = &*(end - 1);
    } else {
        const ViewportCaretEdge& right = *lower;
        const ViewportCaretEdge& left = *(lower - 1);
        const std::uint64_t left_distance = absolute_difference(
            viewport_inline_position,
            left.viewport_inline_position);
        const std::uint64_t right_distance = absolute_difference(
            viewport_inline_position,
            right.viewport_inline_position);
        if (left_distance < right_distance) {
            chosen = &left;
        } else if (right_distance < left_distance) {
            chosen = &right;
        } else if (bias == ViewportHitTestBias::TowardVisualEnd) {
            chosen = &right;
        } else {
            chosen = &left;
        }
    }

    output->source_line_index = line.source_line_index;
    output->source_fragment_index =
        chosen->source_fragment_index;
    output->boundary_index = chosen->boundary_index;
    output->inline_distance = absolute_difference(
        viewport_inline_position,
        chosen->viewport_inline_position);
    output->block_distance = best_block_distance;
    const ViewportCaretEdge& first_caret =
        projection.carets[first];
    const ViewportCaretEdge& last_caret =
        projection.carets[limit - 1U];
    if (viewport_inline_position <
            first_caret.viewport_inline_position ||
        viewport_inline_position >
            last_caret.viewport_inline_position) {
        output->flags |= kViewportHitClampedInline;
    }
    if (best_block_distance != 0U) {
        output->flags |= kViewportHitClampedBlock;
    }
    return true;
}

} // namespace zevryon::text
