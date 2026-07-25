#include "line_selection.hpp"

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

std::pmr::memory_resource* usable_resource(
    std::pmr::memory_resource* resource) noexcept {
    return resource != nullptr ? resource : std::pmr::get_default_resource();
}

void clear_error(LineSelectionError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
}

bool fail(
    LineSelectionErrorKind kind,
    std::size_t segment_index,
    std::size_t glyph_index,
    std::uint32_t cluster_index,
    std::uint32_t boundary_index,
    const char* message,
    LineSelectionError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->segment_index = segment_index;
        error->glyph_index = glyph_index;
        error->cluster_index = cluster_index;
        error->boundary_index = boundary_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool checked_add(std::uint64_t* value, std::uint64_t addition) noexcept {
    if (*value > std::numeric_limits<std::uint64_t>::max() - addition) {
        return false;
    }
    *value += addition;
    return true;
}

std::uint64_t advance_magnitude(std::int32_t advance) noexcept {
    const std::int64_t widened = advance;
    return widened >= 0 ? static_cast<std::uint64_t>(widened)
                        : static_cast<std::uint64_t>(-widened);
}

bool same_group(
    const GlyphClusterRecord& left,
    const GlyphClusterRecord& right) noexcept {
    return left.segment_index == right.segment_index &&
           left.owner_cluster == right.owner_cluster &&
           left.first_glyph == right.first_glyph &&
           left.glyph_count == right.glyph_count;
}

bool is_horizontal(ShapingDirection direction) noexcept {
    return direction == ShapingDirection::LeftToRight ||
           direction == ShapingDirection::RightToLeft;
}

struct SelectionPass final {
    std::size_t output_index{0};
    std::uint32_t line_start{0};
    std::uint64_t line_advance{0};
    std::uint32_t fitting_boundary{0};
    std::uint64_t fitting_advance{0};
};

bool record_line(
    std::uint32_t cluster_limit,
    std::uint64_t inline_advance,
    std::uint32_t flags,
    SelectedLineRecord* records,
    SelectionPass* pass,
    LineSelectionStats* stats,
    LineSelectionError* error) noexcept {
    if (cluster_limit < pass->line_start) {
        return fail(
            LineSelectionErrorKind::InconsistentTopology,
            0U,
            0U,
            pass->line_start,
            cluster_limit,
            "selected line moves backward in the logical cluster domain",
            error);
    }

    const std::uint64_t line_clusters =
        static_cast<std::uint64_t>(cluster_limit - pass->line_start);
    if (line_clusters == 0U) {
        flags |= kSelectedLineEmpty;
    }

    if (records != nullptr) {
        records[pass->output_index] = {
            inline_advance,
            cluster_limit,
            flags};
    }
    ++pass->output_index;

    if (!checked_add(&stats->output_lines, 1U) ||
        !checked_add(
            &stats->soft_break_lines,
            (flags & kSelectedLineSoftBreak) != 0U ? 1U : 0U) ||
        !checked_add(
            &stats->mandatory_break_lines,
            (flags & kSelectedLineMandatoryBreak) != 0U ? 1U : 0U) ||
        !checked_add(
            &stats->overflow_lines,
            (flags & kSelectedLineOverflow) != 0U ? 1U : 0U) ||
        !checked_add(
            &stats->empty_lines,
            (flags & kSelectedLineEmpty) != 0U ? 1U : 0U) ||
        !checked_add(&stats->total_inline_advance, inline_advance)) {
        return fail(
            LineSelectionErrorKind::AggregateOverflow,
            0U,
            0U,
            pass->line_start,
            cluster_limit,
            "line-selection statistics overflowed",
            error);
    }

    stats->maximum_line_advance = std::max(
        stats->maximum_line_advance,
        inline_advance);
    stats->maximum_line_clusters = std::max(
        stats->maximum_line_clusters,
        line_clusters);
    if ((flags & kSelectedLineOverflow) != 0U) {
        stats->maximum_overflow_advance = std::max(
            stats->maximum_overflow_advance,
            inline_advance);
    }

    pass->line_start = cluster_limit;
    pass->line_advance = 0U;
    pass->fitting_boundary = cluster_limit;
    pass->fitting_advance = 0U;
    return true;
}

bool run_selection_pass(
    const LineSelectionRequest& request,
    const std::pmr::vector<std::uint64_t>& cluster_advances,
    SelectedLineRecord* records,
    LineSelectionStats* stats,
    LineSelectionError* error) noexcept {
    SelectionPass pass;

    if (request.cluster_count == 0U) {
        if (!checked_add(&stats->legal_boundaries, 1U)) {
            return fail(
                LineSelectionErrorKind::AggregateOverflow,
                0U,
                0U,
                0U,
                0U,
                "legal line-break boundary count overflowed",
                error);
        }
        return record_line(
            0U,
            0U,
            kSelectedLineMandatoryBreak |
                kSelectedLineTextEnd,
            records,
            &pass,
            stats,
            error);
    }

    for (std::uint32_t cluster_index = 0U;
         cluster_index < request.cluster_count;
         ++cluster_index) {
        const std::uint64_t cluster_advance =
            cluster_advances[static_cast<std::size_t>(cluster_index)];
        if (!checked_add(&pass.line_advance, cluster_advance)) {
            return fail(
                LineSelectionErrorKind::AdvanceOverflow,
                0U,
                0U,
                cluster_index,
                cluster_index + 1U,
                "selected line inline advance overflowed",
                error);
        }

        const std::uint32_t boundary_index = cluster_index + 1U;
        const auto opportunity = static_cast<LineBreakOpportunity>(
            request.opportunity_map->opportunities[
                static_cast<std::size_t>(boundary_index)]);
        const std::uint8_t caret_flags = request.caret_map->flags[
            static_cast<std::size_t>(boundary_index)];
        const bool caret_safe =
            (caret_flags & kCaretBoundarySafe) != 0U;

        if (opportunity == LineBreakOpportunity::Mandatory) {
            if (!caret_safe) {
                return fail(
                    LineSelectionErrorKind::InconsistentTopology,
                    0U,
                    0U,
                    cluster_index,
                    boundary_index,
                    "mandatory line break is not a safe glyph boundary",
                    error);
            }
            if (!checked_add(&stats->legal_boundaries, 1U)) {
                return fail(
                    LineSelectionErrorKind::AggregateOverflow,
                    0U,
                    0U,
                    cluster_index,
                    boundary_index,
                    "legal line-break boundary count overflowed",
                    error);
            }

            if (pass.line_advance > request.available_inline_advance &&
                pass.fitting_boundary > pass.line_start) {
                const std::uint32_t fitting_boundary = pass.fitting_boundary;
                const std::uint64_t fitting_advance = pass.fitting_advance;
                const std::uint64_t remaining_advance =
                    pass.line_advance - fitting_advance;
                if (!record_line(
                        fitting_boundary,
                        fitting_advance,
                        kSelectedLineSoftBreak,
                        records,
                        &pass,
                        stats,
                        error)) {
                    return false;
                }
                pass.line_advance = remaining_advance;
            }

            std::uint32_t flags = kSelectedLineMandatoryBreak;
            if (boundary_index == request.cluster_count) {
                flags |= kSelectedLineTextEnd;
            }
            if (pass.line_advance > request.available_inline_advance) {
                flags |= kSelectedLineOverflow;
            }
            if (!record_line(
                    boundary_index,
                    pass.line_advance,
                    flags,
                    records,
                    &pass,
                    stats,
                    error)) {
                return false;
            }
            continue;
        }

        const bool legal_allowed =
            opportunity == LineBreakOpportunity::Allowed && caret_safe;
        if (legal_allowed &&
            !checked_add(&stats->legal_boundaries, 1U)) {
            return fail(
                LineSelectionErrorKind::AggregateOverflow,
                0U,
                0U,
                cluster_index,
                boundary_index,
                "legal line-break boundary count overflowed",
                error);
        }
        if (opportunity == LineBreakOpportunity::Allowed && !caret_safe &&
            !checked_add(&stats->suppressed_unsafe_boundaries, 1U)) {
            return fail(
                LineSelectionErrorKind::AggregateOverflow,
                0U,
                0U,
                cluster_index,
                boundary_index,
                "suppressed unsafe boundary count overflowed",
                error);
        }

        if (legal_allowed) {
            if (pass.line_advance <= request.available_inline_advance) {
                pass.fitting_boundary = boundary_index;
                pass.fitting_advance = pass.line_advance;
                continue;
            }

            if (pass.fitting_boundary > pass.line_start) {
                const std::uint32_t fitting_boundary = pass.fitting_boundary;
                const std::uint64_t fitting_advance = pass.fitting_advance;
                const std::uint64_t remaining_advance =
                    pass.line_advance - fitting_advance;
                if (!record_line(
                        fitting_boundary,
                        fitting_advance,
                        kSelectedLineSoftBreak,
                        records,
                        &pass,
                        stats,
                        error)) {
                    return false;
                }
                pass.line_advance = remaining_advance;

                if (pass.line_advance <= request.available_inline_advance) {
                    pass.fitting_boundary = boundary_index;
                    pass.fitting_advance = pass.line_advance;
                } else if (!record_line(
                               boundary_index,
                               pass.line_advance,
                               kSelectedLineSoftBreak |
                                   kSelectedLineOverflow,
                               records,
                               &pass,
                               stats,
                               error)) {
                    return false;
                }
                continue;
            }

            if (!record_line(
                    boundary_index,
                    pass.line_advance,
                    kSelectedLineSoftBreak |
                        kSelectedLineOverflow,
                    records,
                    &pass,
                    stats,
                    error)) {
                return false;
            }
            continue;
        }

        if (pass.line_advance > request.available_inline_advance &&
            pass.fitting_boundary > pass.line_start) {
            const std::uint32_t fitting_boundary = pass.fitting_boundary;
            const std::uint64_t fitting_advance = pass.fitting_advance;
            const std::uint64_t remaining_advance =
                pass.line_advance - fitting_advance;
            if (!record_line(
                    fitting_boundary,
                    fitting_advance,
                    kSelectedLineSoftBreak,
                    records,
                    &pass,
                    stats,
                    error)) {
                return false;
            }
            pass.line_advance = remaining_advance;
        }
    }

    if (pass.line_start != request.cluster_count) {
        return fail(
            LineSelectionErrorKind::InconsistentTopology,
            0U,
            0U,
            pass.line_start,
            request.cluster_count,
            "terminal mandatory boundary did not close the cluster domain",
            error);
    }
    return true;
}

} // namespace

LineSelection::LineSelection(std::pmr::memory_resource* resource)
    : lines(usable_resource(resource)) {}

std::pmr::memory_resource* LineSelection::resource() const noexcept {
    return lines.get_allocator().resource();
}

void LineSelection::release() noexcept {
    release_vector(&lines);
}

const char* line_selection_error_kind_name(
    LineSelectionErrorKind kind) noexcept {
    switch (kind) {
        case LineSelectionErrorKind::None:
            return "none";
        case LineSelectionErrorKind::InvalidInput:
            return "invalid_input";
        case LineSelectionErrorKind::InconsistentTopology:
            return "inconsistent_topology";
        case LineSelectionErrorKind::UnsupportedDirection:
            return "unsupported_direction";
        case LineSelectionErrorKind::InvalidGlyphSpan:
            return "invalid_glyph_span";
        case LineSelectionErrorKind::AdvanceOverflow:
            return "advance_overflow";
        case LineSelectionErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
        case LineSelectionErrorKind::AggregateOverflow:
            return "aggregate_overflow";
    }
    return "invalid";
}

std::uint32_t selected_line_first_cluster(
    const LineSelection& selection,
    std::size_t line_index) noexcept {
    if (line_index >= selection.lines.size()) {
        return 0U;
    }
    return line_index == 0U
        ? 0U
        : selection.lines[line_index - 1U].cluster_limit;
}

bool selected_line_has_flag(
    const LineSelection& selection,
    std::size_t line_index,
    SelectedLineFlags flag) noexcept {
    return line_index < selection.lines.size() &&
           (selection.lines[line_index].flags &
            static_cast<std::uint32_t>(flag)) != 0U;
}

bool select_bounded_lines(
    const LineSelectionRequest& request,
    LineSelection* output,
    LineSelectionStats* stats,
    LineSelectionError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    output->release();
    *stats = {};
    clear_error(error);

    if (request.shaped_text == nullptr ||
        request.cluster_map == nullptr ||
        request.caret_map == nullptr ||
        request.opportunity_map == nullptr ||
        request.cluster_count == std::numeric_limits<std::uint32_t>::max()) {
        return fail(
            LineSelectionErrorKind::InvalidInput,
            0U,
            0U,
            0U,
            0U,
            "line selection requires bounded shaping, cluster, caret, and opportunity inputs",
            error);
    }

    const std::size_t cluster_count =
        static_cast<std::size_t>(request.cluster_count);
    const std::size_t boundary_count = cluster_count + 1U;
    stats->input_clusters = request.cluster_count;
    stats->input_boundaries = boundary_count;
    stats->input_segments = request.shaped_text->segments.size();

    if (request.cluster_map->records.size() != cluster_count ||
        request.caret_map->flags.size() != boundary_count ||
        request.opportunity_map->opportunities.size() != boundary_count) {
        return fail(
            LineSelectionErrorKind::InconsistentTopology,
            0U,
            0U,
            0U,
            0U,
            "line-selection inputs do not cover one identical cluster domain",
            error);
    }

    for (std::size_t boundary_index = 0U;
         boundary_index < boundary_count;
         ++boundary_index) {
        if (request.opportunity_map->opportunities[boundary_index] >
            static_cast<std::uint8_t>(LineBreakOpportunity::Mandatory)) {
            return fail(
                LineSelectionErrorKind::InconsistentTopology,
                0U,
                0U,
                0U,
                static_cast<std::uint32_t>(boundary_index),
                "line-break opportunity map contains an invalid classification",
                error);
        }
    }

    if (static_cast<LineBreakOpportunity>(
            request.opportunity_map->opportunities.back()) !=
            LineBreakOpportunity::Mandatory ||
        (request.caret_map->flags.back() & kCaretBoundarySafe) == 0U) {
        return fail(
            LineSelectionErrorKind::InconsistentTopology,
            0U,
            0U,
            request.cluster_count,
            request.cluster_count,
            "terminal boundary must be mandatory and caret-safe",
            error);
    }

    if ((cluster_count == 0U) != request.shaped_text->segments.empty()) {
        return fail(
            LineSelectionErrorKind::InconsistentTopology,
            0U,
            0U,
            0U,
            0U,
            "empty and non-empty shaping topology does not match the cluster domain",
            error);
    }

    std::uint32_t expected_first_cluster = 0U;
    for (std::size_t segment_index = 0U;
         segment_index < request.shaped_text->segments.size();
         ++segment_index) {
        const MultiRunShapedSegment& segment =
            request.shaped_text->segments[segment_index];
        const ShapedGlyphRun& run = segment.glyphs;
        if (segment_index > static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max()) ||
            run.first_cluster != expected_first_cluster ||
            run.first_cluster != segment.run.cluster_index ||
            run.cluster_limit <= run.first_cluster ||
            run.cluster_limit > request.cluster_count ||
            run.direction != segment.run.direction ||
            run.script != segment.run.script ||
            !is_horizontal(run.direction) ||
            run.glyphs.empty()) {
            return fail(
                is_horizontal(run.direction)
                    ? LineSelectionErrorKind::InconsistentTopology
                    : LineSelectionErrorKind::UnsupportedDirection,
                segment_index,
                0U,
                run.first_cluster,
                run.first_cluster,
                "shaped segments must form contiguous horizontal logical runs",
                error);
        }
        if (!checked_add(
                &stats->input_glyphs,
                static_cast<std::uint64_t>(run.glyphs.size()))) {
            return fail(
                LineSelectionErrorKind::AggregateOverflow,
                segment_index,
                0U,
                run.first_cluster,
                run.first_cluster,
                "input glyph count overflowed",
                error);
        }
        expected_first_cluster = run.cluster_limit;
    }
    if (expected_first_cluster != request.cluster_count) {
        return fail(
            LineSelectionErrorKind::InconsistentTopology,
            request.shaped_text->segments.size(),
            0U,
            expected_first_cluster,
            expected_first_cluster,
            "shaped segments do not cover the complete cluster domain",
            error);
    }

    std::pmr::vector<std::uint64_t> cluster_advances(output->resource());
    try {
        cluster_advances.resize(cluster_count, 0U);
    } catch (const std::bad_alloc&) {
        return fail(
            LineSelectionErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            0U,
            "cluster-advance working set exceeds its hard budget",
            error);
    } catch (...) {
        return fail(
            LineSelectionErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            0U,
            "cluster-advance working-set allocation failed",
            error);
    }

    for (std::uint32_t cluster_index = 0U;
         cluster_index < request.cluster_count;
         ++cluster_index) {
        const GlyphClusterRecord& record = request.cluster_map->records[
            static_cast<std::size_t>(cluster_index)];
        if (record.segment_index >= request.shaped_text->segments.size() ||
            record.owner_cluster >= request.cluster_count ||
            record.glyph_count == 0U) {
            return fail(
                LineSelectionErrorKind::InconsistentTopology,
                record.segment_index,
                record.first_glyph,
                cluster_index,
                cluster_index,
                "glyph-cluster record references an invalid owner or segment",
                error);
        }

        const GlyphClusterRecord& owner = request.cluster_map->records[
            static_cast<std::size_t>(record.owner_cluster)];
        if (owner.owner_cluster != record.owner_cluster ||
            !same_group(record, owner)) {
            return fail(
                LineSelectionErrorKind::InconsistentTopology,
                record.segment_index,
                record.first_glyph,
                cluster_index,
                cluster_index,
                "glyph-cluster continuation does not match its owner group",
                error);
        }

        const MultiRunShapedSegment& segment =
            request.shaped_text->segments[record.segment_index];
        const ShapedGlyphRun& run = segment.glyphs;
        const std::size_t first_glyph = record.first_glyph;
        const std::size_t glyph_count = record.glyph_count;
        if (cluster_index < run.first_cluster ||
            cluster_index >= run.cluster_limit ||
            record.owner_cluster < run.first_cluster ||
            record.owner_cluster >= run.cluster_limit ||
            first_glyph > run.glyphs.size() ||
            glyph_count > run.glyphs.size() - first_glyph) {
            return fail(
                LineSelectionErrorKind::InvalidGlyphSpan,
                record.segment_index,
                first_glyph,
                cluster_index,
                cluster_index,
                "glyph-cluster record references an invalid segment-local glyph span",
                error);
        }

        if (record.owner_cluster != cluster_index) {
            if (!checked_add(&stats->zero_advance_clusters, 1U)) {
                return fail(
                    LineSelectionErrorKind::AggregateOverflow,
                    record.segment_index,
                    first_glyph,
                    cluster_index,
                    cluster_index,
                    "zero-advance cluster count overflowed",
                    error);
            }
            continue;
        }

        std::uint64_t owner_advance = 0U;
        for (std::size_t glyph_offset = 0U;
             glyph_offset < glyph_count;
             ++glyph_offset) {
            const std::size_t glyph_index = first_glyph + glyph_offset;
            if (!checked_add(
                    &owner_advance,
                    advance_magnitude(run.glyphs[glyph_index].x_advance))) {
                return fail(
                    LineSelectionErrorKind::AdvanceOverflow,
                    record.segment_index,
                    glyph_index,
                    cluster_index,
                    cluster_index,
                    "glyph-group inline advance overflowed",
                    error);
            }
        }
        cluster_advances[static_cast<std::size_t>(cluster_index)] =
            owner_advance;
        if (owner_advance == 0U &&
            !checked_add(&stats->zero_advance_clusters, 1U)) {
            return fail(
                LineSelectionErrorKind::AggregateOverflow,
                record.segment_index,
                first_glyph,
                cluster_index,
                cluster_index,
                "zero-advance cluster count overflowed",
                error);
        }
    }

    LineSelectionStats first_pass_stats = *stats;
    if (!run_selection_pass(
            request,
            cluster_advances,
            nullptr,
            &first_pass_stats,
            error)) {
        return false;
    }
    if (first_pass_stats.output_lines >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max())) {
        return fail(
            LineSelectionErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            0U,
            "selected-line count exceeds the addressable output domain",
            error);
    }

    LineSelection working(output->resource());
    try {
        working.lines.resize(
            static_cast<std::size_t>(first_pass_stats.output_lines));
    } catch (const std::bad_alloc&) {
        return fail(
            LineSelectionErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            0U,
            "selected-line output exceeds its hard budget",
            error);
    } catch (...) {
        return fail(
            LineSelectionErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            0U,
            "selected-line output allocation failed",
            error);
    }

    LineSelectionStats second_pass_stats = *stats;
    if (!run_selection_pass(
            request,
            cluster_advances,
            working.lines.data(),
            &second_pass_stats,
            error)) {
        return false;
    }
    if (second_pass_stats.output_lines != first_pass_stats.output_lines ||
        second_pass_stats.total_inline_advance !=
            first_pass_stats.total_inline_advance ||
        second_pass_stats.maximum_line_advance !=
            first_pass_stats.maximum_line_advance) {
        return fail(
            LineSelectionErrorKind::InconsistentTopology,
            0U,
            0U,
            0U,
            0U,
            "line-selection passes produced inconsistent deterministic results",
            error);
    }

    working.lines.swap(output->lines);
    *stats = second_pass_stats;
    return true;
}

} // namespace zevryon::text
