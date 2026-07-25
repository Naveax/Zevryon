#include "caret_boundary_map.hpp"

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

void clear_error(CaretBoundaryMapError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
}

bool fail(
    CaretBoundaryMapErrorKind kind,
    std::size_t boundary_index,
    std::size_t segment_index,
    std::size_t glyph_index,
    const char* message,
    CaretBoundaryMapError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->boundary_index = boundary_index;
        error->segment_index = segment_index;
        error->glyph_index = glyph_index;
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

bool same_group(
    const GlyphClusterRecord& left,
    const GlyphClusterRecord& right) noexcept {
    return left == right;
}

bool validate_and_measure_group(
    const MultiRunShapedText& shaped_text,
    const GlyphClusterMap& cluster_map,
    std::size_t first_cluster,
    std::size_t cluster_limit,
    bool* unsafe_to_break,
    std::uint64_t* mapped_glyphs,
    CaretBoundaryMapStats* stats,
    CaretBoundaryMapError* error) noexcept {
    const GlyphClusterRecord& record = cluster_map.records[first_cluster];
    if (record.owner_cluster != first_cluster ||
        record.segment_index >= shaped_text.segments.size()) {
        return fail(
            CaretBoundaryMapErrorKind::InconsistentClusterMap,
            first_cluster,
            record.segment_index,
            record.first_glyph,
            "cluster group does not have a valid logical owner segment",
            error);
    }

    const MultiRunShapedSegment& segment =
        shaped_text.segments[record.segment_index];
    const ShapedGlyphRun& run = segment.glyphs;
    if (first_cluster < run.first_cluster ||
        cluster_limit > run.cluster_limit ||
        record.glyph_count == 0U ||
        record.first_glyph >= run.glyphs.size() ||
        record.glyph_count > run.glyphs.size() - record.first_glyph) {
        return fail(
            CaretBoundaryMapErrorKind::InvalidGlyphSpan,
            first_cluster,
            record.segment_index,
            record.first_glyph,
            "cluster group references a glyph span outside its retained segment",
            error);
    }

    *unsafe_to_break = false;
    const std::size_t glyph_limit =
        static_cast<std::size_t>(record.first_glyph) + record.glyph_count;
    for (std::size_t glyph_index = record.first_glyph;
         glyph_index < glyph_limit;
         ++glyph_index) {
        const ShapedGlyph& glyph = run.glyphs[glyph_index];
        if (glyph.cluster_index != record.owner_cluster) {
            return fail(
                CaretBoundaryMapErrorKind::InvalidGlyphSpan,
                first_cluster,
                record.segment_index,
                glyph_index,
                "glyph span does not belong to the declared logical owner",
                error);
        }
        if ((glyph.flags & kShapedGlyphUnsafeToBreak) != 0U) {
            *unsafe_to_break = true;
        }
    }

    if (!checked_add(&stats->glyph_groups, 1U) ||
        !checked_add(mapped_glyphs, record.glyph_count)) {
        return fail(
            CaretBoundaryMapErrorKind::AggregateOverflow,
            first_cluster,
            record.segment_index,
            record.first_glyph,
            "caret-map aggregate statistics overflowed",
            error);
    }
    return true;
}

bool count_boundary(
    std::uint8_t flags,
    CaretBoundaryMapStats* stats,
    CaretBoundaryMapError* error,
    std::size_t boundary_index) noexcept {
    const bool safe = (flags & kCaretBoundarySafe) != 0U;
    if (!checked_add(
            safe ? &stats->safe_boundaries : &stats->unsafe_boundaries,
            1U) ||
        !checked_add(
            &stats->text_edge_boundaries,
            (flags & kCaretBoundaryTextEdge) != 0U ? 1U : 0U) ||
        !checked_add(
            &stats->run_edge_boundaries,
            (flags & kCaretBoundaryRunEdge) != 0U ? 1U : 0U) ||
        !checked_add(
            &stats->glyph_edge_boundaries,
            (flags & kCaretBoundaryGlyphEdge) != 0U ? 1U : 0U) ||
        !checked_add(
            &stats->merged_interior_boundaries,
            (flags & kCaretBoundaryInsideMergedGroup) != 0U ? 1U : 0U) ||
        !checked_add(
            &stats->unsafe_to_break_boundaries,
            (flags & kCaretBoundaryUnsafeToBreak) != 0U ? 1U : 0U)) {
        return fail(
            CaretBoundaryMapErrorKind::AggregateOverflow,
            boundary_index,
            0U,
            0U,
            "caret-boundary statistics overflowed",
            error);
    }
    return true;
}

} // namespace

CaretBoundaryMap::CaretBoundaryMap(std::pmr::memory_resource* resource)
    : flags(usable_resource(resource)) {}

std::pmr::memory_resource* CaretBoundaryMap::resource() const noexcept {
    return flags.get_allocator().resource();
}

void CaretBoundaryMap::release() noexcept {
    release_vector(&flags);
}

const char* caret_boundary_map_error_kind_name(
    CaretBoundaryMapErrorKind kind) noexcept {
    switch (kind) {
        case CaretBoundaryMapErrorKind::None:
            return "none";
        case CaretBoundaryMapErrorKind::InvalidInput:
            return "invalid_input";
        case CaretBoundaryMapErrorKind::InconsistentClusterMap:
            return "inconsistent_cluster_map";
        case CaretBoundaryMapErrorKind::InvalidGlyphSpan:
            return "invalid_glyph_span";
        case CaretBoundaryMapErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
        case CaretBoundaryMapErrorKind::AggregateOverflow:
            return "aggregate_overflow";
    }
    return "invalid";
}

bool caret_boundary_has_flag(
    const CaretBoundaryMap& map,
    std::uint32_t boundary_index,
    CaretBoundaryFlags flag) noexcept {
    const std::size_t index = static_cast<std::size_t>(boundary_index);
    return index < map.flags.size() &&
           (map.flags[index] & static_cast<std::uint8_t>(flag)) != 0U;
}

bool build_caret_boundary_map(
    const CaretBoundaryMapRequest& request,
    CaretBoundaryMap* output,
    CaretBoundaryMapStats* stats,
    CaretBoundaryMapError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    output->release();
    *stats = {};
    clear_error(error);

    if (request.shaped_text == nullptr || request.cluster_map == nullptr ||
        request.cluster_count == std::numeric_limits<std::uint32_t>::max()) {
        return fail(
            CaretBoundaryMapErrorKind::InvalidInput,
            0U,
            0U,
            0U,
            "caret mapping requires bounded shaped text and cluster-map inputs",
            error);
    }

    const MultiRunShapedText& shaped_text = *request.shaped_text;
    const GlyphClusterMap& cluster_map = *request.cluster_map;
    if (cluster_map.records.size() != request.cluster_count) {
        return fail(
            CaretBoundaryMapErrorKind::InconsistentClusterMap,
            0U,
            0U,
            0U,
            "cluster-map size does not match the requested logical domain",
            error);
    }
    if ((request.cluster_count == 0U) != shaped_text.segments.empty()) {
        return fail(
            CaretBoundaryMapErrorKind::InvalidInput,
            0U,
            0U,
            0U,
            "empty text, segment, and cluster-map domains must agree",
            error);
    }

    stats->input_segments = shaped_text.segments.size();
    stats->input_clusters = request.cluster_count;
    for (const MultiRunShapedSegment& segment : shaped_text.segments) {
        if (!checked_add(
                &stats->input_glyphs,
                static_cast<std::uint64_t>(segment.glyphs.glyphs.size()))) {
            return fail(
                CaretBoundaryMapErrorKind::AggregateOverflow,
                0U,
                0U,
                0U,
                "input glyph count overflows the caret-map contract",
                error);
        }
    }
    CaretBoundaryMap working(output->resource());
    const std::size_t boundary_count =
        static_cast<std::size_t>(request.cluster_count) + 1U;
    try {
        working.flags.resize(boundary_count, 0U);
    } catch (const std::bad_alloc&) {
        return fail(
            CaretBoundaryMapErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            "caret-boundary map exceeds its hard budget",
            error);
    } catch (...) {
        return fail(
            CaretBoundaryMapErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            "caret-boundary map allocation failed",
            error);
    }

    const std::uint8_t text_edge_flags =
        kCaretBoundarySafe |
        kCaretBoundaryTextEdge |
        kCaretBoundaryRunEdge |
        kCaretBoundaryGlyphEdge;
    working.flags.front() = text_edge_flags;
    if (boundary_count > 1U) {
        working.flags.back() = text_edge_flags;
    }

    if (request.cluster_count != 0U) {
        std::size_t group_first = 0U;
        bool previous_group_unsafe = false;
        std::uint32_t previous_segment = 0U;
        bool have_previous_group = false;
        std::uint64_t mapped_glyphs = 0U;

        while (group_first < cluster_map.records.size()) {
            const GlyphClusterRecord& record = cluster_map.records[group_first];
            std::size_t group_limit = group_first + 1U;
            while (group_limit < cluster_map.records.size() &&
                   same_group(record, cluster_map.records[group_limit])) {
                ++group_limit;
            }

            bool group_unsafe = false;
            if (!validate_and_measure_group(
                    shaped_text,
                    cluster_map,
                    group_first,
                    group_limit,
                    &group_unsafe,
                    &mapped_glyphs,
                    stats,
                    error)) {
                return false;
            }

            for (std::size_t boundary = group_first + 1U;
                 boundary < group_limit;
                 ++boundary) {
                working.flags[boundary] = kCaretBoundaryInsideMergedGroup;
            }

            if (!have_previous_group) {
                if (record.segment_index != 0U || group_first != 0U) {
                    return fail(
                        CaretBoundaryMapErrorKind::InconsistentClusterMap,
                        group_first,
                        record.segment_index,
                        record.first_glyph,
                        "first logical group must begin in the first shaped segment",
                        error);
                }
            } else {
                if (record.segment_index < previous_segment ||
                    record.segment_index > previous_segment + 1U) {
                    return fail(
                        CaretBoundaryMapErrorKind::InconsistentClusterMap,
                        group_first,
                        record.segment_index,
                        record.first_glyph,
                        "logical groups must traverse shaped segments monotonically",
                        error);
                }
                std::uint8_t boundary_flags = kCaretBoundaryGlyphEdge;
                if (record.segment_index != previous_segment) {
                    const ShapedGlyphRun& previous_run =
                        shaped_text.segments[previous_segment].glyphs;
                    const ShapedGlyphRun& current_run =
                        shaped_text.segments[record.segment_index].glyphs;
                    if (previous_run.cluster_limit != group_first ||
                        current_run.first_cluster != group_first) {
                        return fail(
                            CaretBoundaryMapErrorKind::InconsistentClusterMap,
                            group_first,
                            record.segment_index,
                            record.first_glyph,
                            "segment transition does not match the logical boundary",
                            error);
                    }
                    boundary_flags |= kCaretBoundaryRunEdge;
                }
                if (previous_group_unsafe || group_unsafe) {
                    boundary_flags |= kCaretBoundaryUnsafeToBreak;
                } else {
                    boundary_flags |= kCaretBoundarySafe;
                }
                working.flags[group_first] = boundary_flags;
            }

            previous_group_unsafe = group_unsafe;
            previous_segment = record.segment_index;
            have_previous_group = true;
            group_first = group_limit;
        }

        if (previous_segment + 1U != shaped_text.segments.size() ||
            shaped_text.segments[previous_segment].glyphs.cluster_limit !=
                request.cluster_count ||
            mapped_glyphs != stats->input_glyphs) {
            return fail(
                CaretBoundaryMapErrorKind::InconsistentClusterMap,
                request.cluster_count,
                previous_segment,
                0U,
                "cluster map does not cover every retained shaped segment and glyph",
                error);
        }
    }

    for (std::size_t boundary = 0U;
         boundary < working.flags.size();
         ++boundary) {
        if (!count_boundary(working.flags[boundary], stats, error, boundary)) {
            return false;
        }
    }

    stats->output_boundaries = working.flags.size();
    output->flags.swap(working.flags);
    return true;
}

} // namespace zevryon::text
