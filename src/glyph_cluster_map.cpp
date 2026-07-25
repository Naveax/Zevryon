#include "glyph_cluster_map.hpp"

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

void clear_error(GlyphClusterMapError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
}

bool fail(
    GlyphClusterMapErrorKind kind,
    std::size_t segment_index,
    std::size_t glyph_index,
    std::uint32_t cluster_index,
    const char* message,
    GlyphClusterMap* output,
    GlyphClusterMapError* error) noexcept {
    if (output != nullptr) {
        output->release();
    }
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

bool fill_owner_span(
    std::uint32_t first_cluster,
    std::uint32_t cluster_limit,
    std::size_t segment_index,
    std::size_t first_glyph,
    std::size_t glyph_count,
    GlyphClusterMap* output,
    GlyphClusterMapStats* stats,
    GlyphClusterMapError* error) noexcept {
    if (first_cluster >= cluster_limit ||
        segment_index > std::numeric_limits<std::uint32_t>::max() ||
        first_glyph > std::numeric_limits<std::uint32_t>::max() ||
        glyph_count == 0U ||
        glyph_count > std::numeric_limits<std::uint32_t>::max()) {
        return fail(
            GlyphClusterMapErrorKind::SegmentTopologyViolation,
            segment_index,
            first_glyph,
            first_cluster,
            "glyph owner span is outside the compact record contract",
            output,
            error);
    }

    const GlyphClusterRecord record{
        static_cast<std::uint32_t>(segment_index),
        first_cluster,
        static_cast<std::uint32_t>(first_glyph),
        static_cast<std::uint32_t>(glyph_count)};
    for (std::uint32_t cluster = first_cluster;
         cluster < cluster_limit;
         ++cluster) {
        output->records[cluster] = record;
    }

    const std::uint64_t span = cluster_limit - first_cluster;
    ++stats->owner_clusters;
    stats->continuation_clusters += span - 1U;
    stats->maximum_group_glyphs = std::max(
        stats->maximum_group_glyphs,
        static_cast<std::uint64_t>(glyph_count));
    stats->maximum_owner_span_clusters = std::max(
        stats->maximum_owner_span_clusters,
        span);
    return true;
}

bool build_ltr_segment(
    const MultiRunShapedSegment& segment,
    std::size_t segment_index,
    GlyphClusterMap* output,
    GlyphClusterMapStats* stats,
    GlyphClusterMapError* error) noexcept {
    const auto& glyphs = segment.glyphs.glyphs;
    std::size_t group_first = 0U;
    std::uint32_t owner = glyphs.front().cluster_index;
    if (owner != segment.glyphs.first_cluster) {
        return fail(
            GlyphClusterMapErrorKind::SegmentTopologyViolation,
            segment_index,
            0U,
            owner,
            "LTR glyph clusters do not begin at the logical run start",
            output,
            error);
    }

    for (std::size_t index = 1U; index <= glyphs.size(); ++index) {
        if (index != glyphs.size() &&
            glyphs[index].cluster_index == owner) {
            continue;
        }
        const std::uint32_t limit = index == glyphs.size()
            ? segment.glyphs.cluster_limit
            : glyphs[index].cluster_index;
        if (limit <= owner ||
            !fill_owner_span(
                owner,
                limit,
                segment_index,
                group_first,
                index - group_first,
                output,
                stats,
                error)) {
            return false;
        }
        if (index != glyphs.size()) {
            group_first = index;
            owner = glyphs[index].cluster_index;
        }
    }
    return true;
}

bool build_rtl_segment(
    const MultiRunShapedSegment& segment,
    std::size_t segment_index,
    GlyphClusterMap* output,
    GlyphClusterMapStats* stats,
    GlyphClusterMapError* error) noexcept {
    const auto& glyphs = segment.glyphs.glyphs;
    std::size_t group_limit = glyphs.size();
    std::size_t group_first = group_limit - 1U;
    std::uint32_t owner = glyphs[group_first].cluster_index;
    while (group_first != 0U &&
           glyphs[group_first - 1U].cluster_index == owner) {
        --group_first;
    }
    if (owner != segment.glyphs.first_cluster) {
        return fail(
            GlyphClusterMapErrorKind::SegmentTopologyViolation,
            segment_index,
            group_first,
            owner,
            "RTL glyph clusters do not cover the logical run start",
            output,
            error);
    }

    while (true) {
        std::size_t next_group_limit = group_first;
        std::size_t next_group_first = 0U;
        std::uint32_t limit = segment.glyphs.cluster_limit;
        if (next_group_limit != 0U) {
            next_group_first = next_group_limit - 1U;
            limit = glyphs[next_group_first].cluster_index;
            while (next_group_first != 0U &&
                   glyphs[next_group_first - 1U].cluster_index == limit) {
                --next_group_first;
            }
        }
        if (limit <= owner ||
            !fill_owner_span(
                owner,
                limit,
                segment_index,
                group_first,
                group_limit - group_first,
                output,
                stats,
                error)) {
            return false;
        }
        if (next_group_limit == 0U) {
            break;
        }
        group_limit = next_group_limit;
        group_first = next_group_first;
        owner = limit;
    }
    return true;
}

} // namespace

GlyphClusterMap::GlyphClusterMap(std::pmr::memory_resource* resource)
    : records(resource != nullptr ? resource : std::pmr::get_default_resource()) {}

std::pmr::memory_resource* GlyphClusterMap::resource() const noexcept {
    return records.get_allocator().resource();
}

void GlyphClusterMap::release() noexcept {
    release_vector(&records);
}

const char* glyph_cluster_map_error_kind_name(
    GlyphClusterMapErrorKind kind) noexcept {
    switch (kind) {
        case GlyphClusterMapErrorKind::None:
            return "none";
        case GlyphClusterMapErrorKind::InvalidInput:
            return "invalid_input";
        case GlyphClusterMapErrorKind::SegmentTopologyViolation:
            return "segment_topology_violation";
        case GlyphClusterMapErrorKind::InvalidGlyphCluster:
            return "invalid_glyph_cluster";
        case GlyphClusterMapErrorKind::NonMonotoneGlyphClusters:
            return "non_monotone_glyph_clusters";
        case GlyphClusterMapErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
    }
    return "invalid";
}

bool build_glyph_cluster_map(
    const MultiRunShapedText& shaped_text,
    std::uint32_t cluster_count,
    GlyphClusterMap* output,
    GlyphClusterMapStats* stats,
    GlyphClusterMapError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    output->release();
    *stats = {};
    clear_error(error);

    if (cluster_count == 0U) {
        if (!shaped_text.segments.empty()) {
            return fail(
                GlyphClusterMapErrorKind::InvalidInput,
                0U,
                0U,
                0U,
                "empty cluster domain requires empty shaped text",
                output,
                error);
        }
        return true;
    }
    if (shaped_text.segments.empty() ||
        shaped_text.segments.size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            GlyphClusterMapErrorKind::InvalidInput,
            0U,
            0U,
            0U,
            "non-empty cluster domain requires compact shaped segments",
            output,
            error);
    }

    try {
        output->records.resize(cluster_count);
    } catch (const std::bad_alloc&) {
        return fail(
            GlyphClusterMapErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            "glyph-cluster map exceeds its hard budget",
            output,
            error);
    } catch (...) {
        return fail(
            GlyphClusterMapErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            "glyph-cluster map allocation failed",
            output,
            error);
    }

    stats->input_segments = shaped_text.segments.size();
    stats->input_clusters = cluster_count;
    std::uint32_t expected_first = 0U;
    for (std::size_t segment_index = 0U;
         segment_index < shaped_text.segments.size();
         ++segment_index) {
        const MultiRunShapedSegment& segment =
            shaped_text.segments[segment_index];
        const ShapedGlyphRun& run = segment.glyphs;
        if (segment.run.cluster_index != run.first_cluster ||
            run.first_cluster != expected_first ||
            run.first_cluster >= run.cluster_limit ||
            run.cluster_limit > cluster_count ||
            segment.run.direction != run.direction ||
            segment.run.script != run.script ||
            run.glyphs.empty()) {
            return fail(
                GlyphClusterMapErrorKind::SegmentTopologyViolation,
                segment_index,
                0U,
                run.first_cluster,
                "shaped segments do not form one contiguous logical cluster partition",
                output,
                error);
        }
        expected_first = run.cluster_limit;
        stats->input_glyphs += run.glyphs.size();

        std::uint32_t previous = run.glyphs.front().cluster_index;
        for (std::size_t glyph_index = 0U;
             glyph_index < run.glyphs.size();
             ++glyph_index) {
            const std::uint32_t cluster =
                run.glyphs[glyph_index].cluster_index;
            if (cluster < run.first_cluster || cluster >= run.cluster_limit) {
                return fail(
                    GlyphClusterMapErrorKind::InvalidGlyphCluster,
                    segment_index,
                    glyph_index,
                    cluster,
                    "glyph cluster lies outside its shaped segment",
                    output,
                    error);
            }
            if (glyph_index != 0U) {
                const bool monotone = run.direction ==
                        ShapingDirection::LeftToRight
                    ? cluster >= previous
                    : cluster <= previous;
                if (!monotone) {
                    return fail(
                        GlyphClusterMapErrorKind::NonMonotoneGlyphClusters,
                        segment_index,
                        glyph_index,
                        cluster,
                        "glyph clusters violate HarfBuzz monotone-grapheme ordering",
                        output,
                        error);
                }
            }
            previous = cluster;
        }

        if (run.direction == ShapingDirection::LeftToRight) {
            ++stats->left_to_right_segments;
            if (!build_ltr_segment(
                    segment,
                    segment_index,
                    output,
                    stats,
                    error)) {
                return false;
            }
        } else if (run.direction == ShapingDirection::RightToLeft) {
            ++stats->right_to_left_segments;
            if (!build_rtl_segment(
                    segment,
                    segment_index,
                    output,
                    stats,
                    error)) {
                return false;
            }
        } else {
            return fail(
                GlyphClusterMapErrorKind::SegmentTopologyViolation,
                segment_index,
                0U,
                run.first_cluster,
                "glyph-cluster indexing currently requires horizontal shaping",
                output,
                error);
        }
    }

    if (expected_first != cluster_count) {
        return fail(
            GlyphClusterMapErrorKind::SegmentTopologyViolation,
            shaped_text.segments.size(),
            0U,
            expected_first,
            "shaped segments do not reach the cluster-domain sentinel",
            output,
            error);
    }
    stats->output_records = output->records.size();
    return true;
}

} // namespace zevryon::text
