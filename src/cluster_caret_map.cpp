#include "cluster_caret_map.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <span>

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

void clear_error(ClusterCaretMapError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
}

bool fail(
    ClusterCaretMapErrorKind kind,
    std::size_t segment_index,
    std::size_t glyph_index,
    std::uint32_t cluster_index,
    const char* message,
    ClusterCaretMapError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->segment_index = segment_index;
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

bool checked_add(std::uint64_t* value, std::uint64_t addition) noexcept {
    if (*value > std::numeric_limits<std::uint64_t>::max() - addition) {
        return false;
    }
    *value += addition;
    return true;
}

bool same_group(
    const ClusterGlyphMapEntry& left,
    const ClusterGlyphMapEntry& right) noexcept {
    return left.segment_index == right.segment_index &&
           left.first_glyph == right.first_glyph &&
           left.glyph_count == right.glyph_count &&
           left.group_first_cluster == right.group_first_cluster &&
           left.group_cluster_limit == right.group_cluster_limit;
}

bool publish_group(
    std::pmr::vector<ClusterGlyphMapEntry>* entries,
    std::uint32_t segment_index,
    std::span<const ShapedGlyph> glyphs,
    std::size_t first_glyph,
    std::size_t glyph_limit,
    std::uint32_t first_cluster,
    std::uint32_t cluster_limit,
    ShapingDirection direction,
    ClusterCaretMapStats* stats,
    ClusterCaretMapError* error) noexcept {
    if (first_glyph >= glyph_limit ||
        glyph_limit > static_cast<std::size_t>(
            std::numeric_limits<std::uint32_t>::max()) ||
        first_cluster >= cluster_limit ||
        static_cast<std::size_t>(cluster_limit) > entries->size()) {
        return fail(
            ClusterCaretMapErrorKind::ClusterCoverageFailure,
            segment_index,
            first_glyph,
            first_cluster,
            "glyph group does not describe a valid cluster interval",
            error);
    }

    std::uint32_t flags = direction == ShapingDirection::RightToLeft
        ? kClusterGlyphMapRightToLeft
        : 0U;
    bool unsafe_to_break = false;
    bool missing_glyph = false;
    for (std::size_t glyph_index = first_glyph;
         glyph_index < glyph_limit;
         ++glyph_index) {
        const ShapedGlyph& glyph = glyphs[glyph_index];
        if ((glyph.flags & kShapedGlyphUnsafeToBreak) != 0U) {
            flags |= kClusterGlyphMapUnsafeToBreak;
            unsafe_to_break = true;
        }
        if ((glyph.flags & kShapedGlyphUnsafeToConcat) != 0U) {
            flags |= kClusterGlyphMapUnsafeToConcat;
        }
        if ((glyph.flags & kShapedGlyphSafeToInsertTatweel) != 0U) {
            flags |= kClusterGlyphMapSafeToInsertTatweel;
        }
        if (glyph.glyph_id == 0U) {
            flags |= kClusterGlyphMapMissingGlyph;
            missing_glyph = true;
        }
    }

    const std::uint32_t group_cluster_count = cluster_limit - first_cluster;
    if (group_cluster_count > 1U) {
        flags |= kClusterGlyphMapMergedGroup;
    }
    const std::size_t group_glyph_count = glyph_limit - first_glyph;
    const ClusterGlyphMapEntry entry{
        segment_index,
        static_cast<std::uint32_t>(first_glyph),
        static_cast<std::uint32_t>(group_glyph_count),
        first_cluster,
        cluster_limit,
        flags};

    for (std::uint32_t cluster_index = first_cluster;
         cluster_index < cluster_limit;
         ++cluster_index) {
        ClusterGlyphMapEntry& destination =
            (*entries)[static_cast<std::size_t>(cluster_index)];
        if (destination.segment_index != kInvalidClusterMapIndex) {
            return fail(
                ClusterCaretMapErrorKind::ClusterCoverageFailure,
                segment_index,
                first_glyph,
                cluster_index,
                "two glyph groups overlap the same logical cluster",
                error);
        }
        destination = entry;
    }

    if (!checked_add(&stats->glyph_groups, 1U) ||
        !checked_add(
            &stats->clusters_in_merged_groups,
            group_cluster_count > 1U ? group_cluster_count : 0U) ||
        !checked_add(&stats->merged_groups, group_cluster_count > 1U ? 1U : 0U) ||
        !checked_add(&stats->unsafe_groups, unsafe_to_break ? 1U : 0U) ||
        !checked_add(&stats->missing_glyph_groups, missing_glyph ? 1U : 0U) ||
        !checked_add(
            direction == ShapingDirection::LeftToRight
                ? &stats->left_to_right_groups
                : &stats->right_to_left_groups,
            1U)) {
        return fail(
            ClusterCaretMapErrorKind::AggregateOverflow,
            segment_index,
            first_glyph,
            first_cluster,
            "cluster-map statistics overflowed",
            error);
    }
    stats->maximum_group_glyphs = std::max(
        stats->maximum_group_glyphs,
        group_glyph_count);
    stats->maximum_group_clusters = std::max(
        stats->maximum_group_clusters,
        group_cluster_count);
    return true;
}

} // namespace

ClusterCaretMap::ClusterCaretMap(std::pmr::memory_resource* resource)
    : clusters(usable_resource(resource)) {}

std::pmr::memory_resource* ClusterCaretMap::resource() const noexcept {
    return clusters.get_allocator().resource();
}

void ClusterCaretMap::release() noexcept {
    release_vector(&clusters);
}

const char* cluster_caret_map_error_kind_name(
    ClusterCaretMapErrorKind kind) noexcept {
    switch (kind) {
        case ClusterCaretMapErrorKind::None:
            return "none";
        case ClusterCaretMapErrorKind::InvalidArgument:
            return "invalid_argument";
        case ClusterCaretMapErrorKind::InvalidSegmentTable:
            return "invalid_segment_table";
        case ClusterCaretMapErrorKind::InvalidGlyphCluster:
            return "invalid_glyph_cluster";
        case ClusterCaretMapErrorKind::InvalidGlyphOrder:
            return "invalid_glyph_order";
        case ClusterCaretMapErrorKind::ClusterCoverageFailure:
            return "cluster_coverage_failure";
        case ClusterCaretMapErrorKind::MetadataBudgetExceeded:
            return "metadata_budget_exceeded";
        case ClusterCaretMapErrorKind::AggregateOverflow:
            return "aggregate_overflow";
    }
    return "invalid";
}

bool inspect_caret_boundary(
    const ClusterCaretMap& map,
    std::uint32_t boundary_index,
    CaretBoundaryInfo* output) noexcept {
    if (output == nullptr ||
        static_cast<std::size_t>(boundary_index) > map.clusters.size()) {
        return false;
    }

    *output = {};
    output->boundary_index = boundary_index;
    const std::size_t boundary = static_cast<std::size_t>(boundary_index);
    if (boundary == 0U || boundary == map.clusters.size()) {
        output->left_cluster = boundary == 0U
            ? kInvalidClusterMapIndex
            : boundary_index - 1U;
        output->right_cluster = boundary == map.clusters.size()
            ? kInvalidClusterMapIndex
            : boundary_index;
        output->flags = kCaretBoundarySafe |
                        kCaretBoundaryTextEdge |
                        kCaretBoundaryRunEdge |
                        kCaretBoundaryGlyphEdge;
        return true;
    }

    output->left_cluster = boundary_index - 1U;
    output->right_cluster = boundary_index;
    const ClusterGlyphMapEntry& left = map.clusters[boundary - 1U];
    const ClusterGlyphMapEntry& right = map.clusters[boundary];
    const bool group_edge = !same_group(left, right);
    const bool run_edge = left.segment_index != right.segment_index;
    const bool unsafe =
        (left.flags & kClusterGlyphMapUnsafeToBreak) != 0U ||
        (right.flags & kClusterGlyphMapUnsafeToBreak) != 0U;

    if (group_edge) {
        output->flags |= kCaretBoundaryGlyphEdge;
    } else {
        output->flags |= kCaretBoundaryInsideMergedGroup;
    }
    if (run_edge) {
        output->flags |= kCaretBoundaryRunEdge;
    }
    if (unsafe) {
        output->flags |= kCaretBoundaryUnsafeToBreak;
    }
    if (group_edge && !unsafe) {
        output->flags |= kCaretBoundarySafe;
    }
    return true;
}

bool build_cluster_caret_map(
    const ClusterCaretMapRequest& request,
    ClusterCaretMap* output,
    ClusterCaretMapStats* stats,
    ClusterCaretMapError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    output->release();
    *stats = {};
    clear_error(error);

    if (request.shaped_text == nullptr) {
        return fail(
            ClusterCaretMapErrorKind::InvalidArgument,
            0U,
            0U,
            0U,
            "cluster mapping requires retained multi-run shaping output",
            error);
    }

    const auto& segments = request.shaped_text->segments;
    stats->input_segments = segments.size();
    stats->input_clusters = request.cluster_count;
    if (request.cluster_count == 0U) {
        if (!segments.empty()) {
            return fail(
                ClusterCaretMapErrorKind::InvalidSegmentTable,
                0U,
                0U,
                0U,
                "zero clusters require an empty shaped segment table",
                error);
        }
        stats->safe_caret_boundaries = 1U;
        return true;
    }
    if (segments.empty() ||
        segments.size() > static_cast<std::size_t>(
            std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            ClusterCaretMapErrorKind::InvalidSegmentTable,
            0U,
            0U,
            0U,
            "non-empty cluster domains require a bounded shaped segment table",
            error);
    }

    ClusterCaretMap working(output->resource());
    try {
        working.clusters.resize(request.cluster_count);
    } catch (const std::bad_alloc&) {
        return fail(
            ClusterCaretMapErrorKind::MetadataBudgetExceeded,
            0U,
            0U,
            0U,
            "cluster-to-glyph metadata exceeds its hard budget",
            error);
    } catch (...) {
        return fail(
            ClusterCaretMapErrorKind::MetadataBudgetExceeded,
            0U,
            0U,
            0U,
            "cluster-to-glyph metadata allocation failed",
            error);
    }

    std::uint32_t expected_first_cluster = 0U;
    for (std::size_t segment_index = 0U;
         segment_index < segments.size();
         ++segment_index) {
        const MultiRunShapedSegment& segment = segments[segment_index];
        const ShapedGlyphRun& run = segment.glyphs;
        if (run.first_cluster != expected_first_cluster ||
            run.first_cluster != segment.run.cluster_index ||
            run.cluster_limit <= run.first_cluster ||
            run.cluster_limit > request.cluster_count ||
            run.direction != segment.run.direction ||
            run.script != segment.run.script ||
            (run.direction != ShapingDirection::LeftToRight &&
             run.direction != ShapingDirection::RightToLeft) ||
            run.glyphs.empty() ||
            run.glyphs.size() > static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max())) {
            return fail(
                ClusterCaretMapErrorKind::InvalidSegmentTable,
                segment_index,
                0U,
                run.first_cluster,
                "shaped segments must form one contiguous logical cluster domain",
                error);
        }

        if (!checked_add(
                &stats->input_glyphs,
                static_cast<std::uint64_t>(run.glyphs.size()))) {
            return fail(
                ClusterCaretMapErrorKind::AggregateOverflow,
                segment_index,
                0U,
                run.first_cluster,
                "input glyph count overflows the cluster-map contract",
                error);
        }

        for (std::size_t glyph_index = 0U;
             glyph_index < run.glyphs.size();
             ++glyph_index) {
            const std::uint32_t cluster_index =
                run.glyphs[glyph_index].cluster_index;
            if (cluster_index < run.first_cluster ||
                cluster_index >= run.cluster_limit) {
                return fail(
                    ClusterCaretMapErrorKind::InvalidGlyphCluster,
                    segment_index,
                    glyph_index,
                    cluster_index,
                    "glyph cluster lies outside its retained shaping segment",
                    error);
            }
            if (glyph_index != 0U) {
                const std::uint32_t previous =
                    run.glyphs[glyph_index - 1U].cluster_index;
                const bool monotone = run.direction == ShapingDirection::LeftToRight
                    ? previous <= cluster_index
                    : previous >= cluster_index;
                if (!monotone) {
                    return fail(
                        ClusterCaretMapErrorKind::InvalidGlyphOrder,
                        segment_index,
                        glyph_index,
                        cluster_index,
                        "glyph clusters violate monotone-grapheme ordering",
                        error);
                }
            }
        }

        const std::span<const ShapedGlyph> glyphs(run.glyphs);
        std::size_t first_glyph = 0U;
        if (run.direction == ShapingDirection::LeftToRight) {
            std::uint32_t expected_group_first = run.first_cluster;
            while (first_glyph < glyphs.size()) {
                const std::uint32_t group_first =
                    glyphs[first_glyph].cluster_index;
                std::size_t glyph_limit = first_glyph + 1U;
                while (glyph_limit < glyphs.size() &&
                       glyphs[glyph_limit].cluster_index == group_first) {
                    ++glyph_limit;
                }
                const std::uint32_t group_limit = glyph_limit < glyphs.size()
                    ? glyphs[glyph_limit].cluster_index
                    : run.cluster_limit;
                if (group_first != expected_group_first ||
                    !publish_group(
                        &working.clusters,
                        static_cast<std::uint32_t>(segment_index),
                        glyphs,
                        first_glyph,
                        glyph_limit,
                        group_first,
                        group_limit,
                        run.direction,
                        stats,
                        error)) {
                    if (error->kind == ClusterCaretMapErrorKind::None) {
                        return fail(
                            ClusterCaretMapErrorKind::ClusterCoverageFailure,
                            segment_index,
                            first_glyph,
                            group_first,
                            "LTR glyph groups leave a logical cluster gap",
                            error);
                    }
                    return false;
                }
                expected_group_first = group_limit;
                first_glyph = glyph_limit;
            }
            if (expected_group_first != run.cluster_limit) {
                return fail(
                    ClusterCaretMapErrorKind::ClusterCoverageFailure,
                    segment_index,
                    run.glyphs.size(),
                    expected_group_first,
                    "LTR glyph groups do not cover the complete run",
                    error);
            }
        } else {
            std::uint32_t expected_group_limit = run.cluster_limit;
            while (first_glyph < glyphs.size()) {
                const std::uint32_t group_first =
                    glyphs[first_glyph].cluster_index;
                std::size_t glyph_limit = first_glyph + 1U;
                while (glyph_limit < glyphs.size() &&
                       glyphs[glyph_limit].cluster_index == group_first) {
                    ++glyph_limit;
                }
                if (!publish_group(
                        &working.clusters,
                        static_cast<std::uint32_t>(segment_index),
                        glyphs,
                        first_glyph,
                        glyph_limit,
                        group_first,
                        expected_group_limit,
                        run.direction,
                        stats,
                        error)) {
                    return false;
                }
                expected_group_limit = group_first;
                first_glyph = glyph_limit;
            }
            if (expected_group_limit != run.first_cluster) {
                return fail(
                    ClusterCaretMapErrorKind::ClusterCoverageFailure,
                    segment_index,
                    run.glyphs.size(),
                    expected_group_limit,
                    "RTL glyph groups do not cover the complete run",
                    error);
            }
        }
        expected_first_cluster = run.cluster_limit;
    }

    if (expected_first_cluster != request.cluster_count) {
        return fail(
            ClusterCaretMapErrorKind::ClusterCoverageFailure,
            segments.size(),
            0U,
            expected_first_cluster,
            "shaped segments do not cover the requested cluster domain",
            error);
    }
    for (std::size_t cluster_index = 0U;
         cluster_index < working.clusters.size();
         ++cluster_index) {
        if (working.clusters[cluster_index].segment_index ==
            kInvalidClusterMapIndex) {
            return fail(
                ClusterCaretMapErrorKind::ClusterCoverageFailure,
                segments.size(),
                0U,
                static_cast<std::uint32_t>(cluster_index),
                "one logical cluster has no glyph-group mapping",
                error);
        }
    }

    for (std::uint32_t boundary_index = 0U;
         boundary_index <= request.cluster_count;
         ++boundary_index) {
        CaretBoundaryInfo boundary;
        if (!inspect_caret_boundary(working, boundary_index, &boundary)) {
            return fail(
                ClusterCaretMapErrorKind::ClusterCoverageFailure,
                segments.size(),
                0U,
                boundary_index,
                "caret-boundary inspection failed after map construction",
                error);
        }
        if ((boundary.flags & kCaretBoundarySafe) != 0U) {
            if (!checked_add(&stats->safe_caret_boundaries, 1U)) {
                return fail(
                    ClusterCaretMapErrorKind::AggregateOverflow,
                    segments.size(),
                    0U,
                    boundary_index,
                    "safe caret-boundary count overflowed",
                    error);
            }
        } else if (!checked_add(&stats->unsafe_caret_boundaries, 1U)) {
            return fail(
                ClusterCaretMapErrorKind::AggregateOverflow,
                segments.size(),
                0U,
                boundary_index,
                "unsafe caret-boundary count overflowed",
                error);
        }
    }

    stats->output_entries = working.clusters.size();
    output->clusters.swap(working.clusters);
    return true;
}

} // namespace zevryon::text
