#include "line_fragment_layout.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace zevryon::text {
namespace {

constexpr std::uint8_t kClusterHasActive = 1U << 0U;
constexpr std::uint8_t kClusterL1Adjusted = 1U << 1U;
constexpr std::uint8_t kClusterMixedLevel = 1U << 2U;

struct ClusterVisualMetadata final {
    std::uint8_t level{0};
    std::uint8_t flags{0};
    std::uint16_t reserved{0};
};

static_assert(sizeof(ClusterVisualMetadata) == 4U);

template <typename T>
void release_vector(std::pmr::vector<T>* values) noexcept {
    std::pmr::vector<T> empty(values->get_allocator().resource());
    values->swap(empty);
}

std::pmr::memory_resource* usable_resource(
    std::pmr::memory_resource* resource) noexcept {
    return resource != nullptr ? resource : std::pmr::get_default_resource();
}

void clear_error(LineFragmentLayoutError* error) noexcept {
    if (error != nullptr) {
        error->kind = LineFragmentLayoutErrorKind::None;
        error->line_index = 0U;
        error->active_index = 0U;
        error->cluster_index = 0U;
        error->segment_index = 0U;
        error->bidi_error.kind = BidiVisualErrorKind::None;
        error->bidi_error.active_index = 0U;
        error->bidi_error.message.clear();
        error->message.clear();
    }
}

bool fail(
    LineFragmentLayoutErrorKind kind,
    std::size_t line_index,
    std::size_t active_index,
    std::uint32_t cluster_index,
    std::uint32_t segment_index,
    const char* message,
    LineFragmentLayoutError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->line_index = line_index;
        error->active_index = active_index;
        error->cluster_index = cluster_index;
        error->segment_index = segment_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool fail_bidi(
    const BidiVisualError& bidi_error,
    LineFragmentLayoutError* error) noexcept {
    if (error != nullptr) {
        error->kind = LineFragmentLayoutErrorKind::BidiVisualFailure;
        error->active_index = bidi_error.active_index;
        try {
            error->bidi_error = bidi_error;
            error->message = "bidi visual-order resolution failed";
        } catch (...) {
            error->bidi_error.kind = bidi_error.kind;
            error->bidi_error.active_index = bidi_error.active_index;
            error->bidi_error.message.clear();
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

bool checked_add_signed(
    std::int64_t* value,
    std::int32_t addition) noexcept {
    const std::int64_t widened = addition;
    if ((widened > 0 &&
         *value > std::numeric_limits<std::int64_t>::max() - widened) ||
        (widened < 0 &&
         *value < std::numeric_limits<std::int64_t>::min() - widened)) {
        return false;
    }
    *value += widened;
    return true;
}

std::uint64_t advance_magnitude(std::int64_t advance) noexcept {
    return advance >= 0
        ? static_cast<std::uint64_t>(advance)
        : static_cast<std::uint64_t>(-(advance + 1)) + 1U;
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

bool is_safe_cluster_boundary(
    const GlyphClusterMap& cluster_map,
    std::uint32_t boundary,
    std::uint32_t cluster_count) noexcept {
    return boundary == 0U || boundary == cluster_count ||
           !same_group(
               cluster_map.records[static_cast<std::size_t>(boundary - 1U)],
               cluster_map.records[static_cast<std::size_t>(boundary)]);
}

std::uint32_t first_cluster_for_line(
    const LineSelection& selection,
    std::size_t line_index) noexcept {
    return line_index == 0U
        ? 0U
        : selection.lines[line_index - 1U].cluster_limit;
}

bool validate_grapheme_and_bidi_inputs(
    const LineFragmentLayoutRequest& request,
    LineFragmentLayoutStats* stats,
    LineFragmentLayoutError* error) noexcept {
    const std::size_t cluster_count = request.cluster_count;
    const std::size_t active_count =
        request.bidi_topology->active_unit_indices.size();
    if (request.paragraph_level > 1U ||
        request.implicit_levels.size() != active_count ||
        active_count > static_cast<std::size_t>(
                           std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            LineFragmentLayoutErrorKind::InvalidInput,
            0U,
            0U,
            0U,
            0U,
            "paragraph level or active bidi input size is invalid",
            error);
    }

    if (cluster_count == 0U) {
        if (!request.grapheme_boundaries.empty() ||
            !request.bidi_units.empty() ||
            active_count != 0U) {
            return fail(
                LineFragmentLayoutErrorKind::TopologyViolation,
                0U,
                0U,
                0U,
                0U,
                "empty cluster input requires empty grapheme and bidi streams",
                error);
        }
        return true;
    }

    if (request.grapheme_boundaries.size() != cluster_count + 1U ||
        request.grapheme_boundaries.front().codepoint_index != 0U ||
        request.grapheme_boundaries.back().codepoint_index !=
            request.bidi_units.size()) {
        return fail(
            LineFragmentLayoutErrorKind::TopologyViolation,
            0U,
            0U,
            0U,
            0U,
            "grapheme boundaries must cover the complete bidi-unit stream",
            error);
    }
    for (std::size_t index = 1U;
         index < request.grapheme_boundaries.size();
         ++index) {
        if (request.grapheme_boundaries[index - 1U].codepoint_index >=
                request.grapheme_boundaries[index].codepoint_index ||
            request.grapheme_boundaries[index - 1U].source_offset >
                request.grapheme_boundaries[index].source_offset) {
            return fail(
                LineFragmentLayoutErrorKind::TopologyViolation,
                0U,
                0U,
                static_cast<std::uint32_t>(index - 1U),
                0U,
                "grapheme boundaries are not strictly ordered",
                error);
        }
    }

    std::uint32_t previous_unit = 0U;
    std::uint32_t previous_codepoint = 0U;
    bool have_previous = false;
    for (std::size_t active = 0U; active < active_count; ++active) {
        const std::uint32_t unit_index =
            request.bidi_topology->active_unit_indices[active];
        if (unit_index >= request.bidi_units.size()) {
            return fail(
                LineFragmentLayoutErrorKind::TopologyViolation,
                0U,
                active,
                0U,
                0U,
                "active bidi index is outside the explicit-unit stream",
                error);
        }
        const BidiExplicitUnit& unit = request.bidi_units[unit_index];
        if ((unit.flags & kBidiUnitRemovedByX9) != 0U ||
            unit.codepoint_index >= request.bidi_units.size() ||
            request.implicit_levels[active] > 126U ||
            request.implicit_levels[active] < unit.level ||
            (have_previous &&
             (unit_index <= previous_unit ||
              unit.codepoint_index <= previous_codepoint))) {
            return fail(
                LineFragmentLayoutErrorKind::TopologyViolation,
                0U,
                active,
                unit.codepoint_index,
                0U,
                "active bidi topology or implicit levels are invalid",
                error);
        }
        previous_unit = unit_index;
        previous_codepoint = unit.codepoint_index;
        have_previous = true;
    }

    stats->input_active_units = active_count;
    return true;
}

bool validate_shaping_and_lines(
    const LineFragmentLayoutRequest& request,
    LineFragmentLayoutStats* stats,
    LineFragmentLayoutError* error) noexcept {
    const std::size_t cluster_count = request.cluster_count;
    if (request.cluster_map->records.size() != cluster_count ||
        request.line_selection->lines.empty() ||
        request.line_selection->lines.size() > static_cast<std::size_t>(
            std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            LineFragmentLayoutErrorKind::TopologyViolation,
            0U,
            0U,
            0U,
            0U,
            "cluster map or selected-line table has an invalid size",
            error);
    }

    std::uint32_t expected_first = 0U;
    for (std::size_t line_index = 0U;
         line_index < request.line_selection->lines.size();
         ++line_index) {
        const SelectedLineRecord& line =
            request.line_selection->lines[line_index];
        if (line.cluster_limit < expected_first ||
            line.cluster_limit > request.cluster_count ||
            (line.cluster_limit == expected_first &&
             (line.flags & kSelectedLineEmpty) == 0U) ||
            !is_safe_cluster_boundary(
                *request.cluster_map,
                expected_first,
                request.cluster_count) ||
            !is_safe_cluster_boundary(
                *request.cluster_map,
                line.cluster_limit,
                request.cluster_count)) {
            return fail(
                LineFragmentLayoutErrorKind::UnsafeFragmentBoundary,
                line_index,
                0U,
                expected_first,
                0U,
                "selected lines do not form safe monotone cluster partitions",
                error);
        }
        expected_first = line.cluster_limit;
    }
    if (expected_first != request.cluster_count) {
        return fail(
            LineFragmentLayoutErrorKind::TopologyViolation,
            request.line_selection->lines.size(),
            0U,
            expected_first,
            0U,
            "selected lines do not cover the complete cluster domain",
            error);
    }

    if ((cluster_count == 0U) != request.shaped_text->segments.empty()) {
        return fail(
            LineFragmentLayoutErrorKind::TopologyViolation,
            0U,
            0U,
            0U,
            0U,
            "empty and non-empty shaping topology does not match the cluster domain",
            error);
    }

    expected_first = 0U;
    for (std::size_t segment_index = 0U;
         segment_index < request.shaped_text->segments.size();
         ++segment_index) {
        const MultiRunShapedSegment& segment =
            request.shaped_text->segments[segment_index];
        const ShapedGlyphRun& run = segment.glyphs;
        if (segment_index > static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max()) ||
            run.first_cluster != expected_first ||
            run.first_cluster != segment.run.cluster_index ||
            run.cluster_limit <= run.first_cluster ||
            run.cluster_limit > request.cluster_count ||
            run.direction != segment.run.direction ||
            run.script != segment.run.script ||
            !is_horizontal(run.direction) ||
            run.glyphs.empty()) {
            return fail(
                LineFragmentLayoutErrorKind::TopologyViolation,
                0U,
                0U,
                run.first_cluster,
                static_cast<std::uint32_t>(segment_index),
                "shaped segments must form contiguous horizontal logical runs",
                error);
        }
        if (!checked_add(
                &stats->input_glyphs,
                static_cast<std::uint64_t>(run.glyphs.size()))) {
            return fail(
                LineFragmentLayoutErrorKind::AggregateOverflow,
                0U,
                0U,
                run.first_cluster,
                static_cast<std::uint32_t>(segment_index),
                "input glyph count overflowed",
                error);
        }
        expected_first = run.cluster_limit;
    }
    if (expected_first != request.cluster_count) {
        return fail(
            LineFragmentLayoutErrorKind::TopologyViolation,
            0U,
            0U,
            expected_first,
            0U,
            "shaped segments do not cover the complete cluster domain",
            error);
    }

    for (std::uint32_t cluster = 0U;
         cluster < request.cluster_count;
         ++cluster) {
        const GlyphClusterRecord& record =
            request.cluster_map->records[static_cast<std::size_t>(cluster)];
        if (record.segment_index >= request.shaped_text->segments.size() ||
            record.owner_cluster >= request.cluster_count ||
            record.glyph_count == 0U) {
            return fail(
                LineFragmentLayoutErrorKind::ClusterMappingViolation,
                0U,
                0U,
                cluster,
                record.segment_index,
                "glyph-cluster record references an invalid owner or segment",
                error);
        }
        const GlyphClusterRecord& owner =
            request.cluster_map->records[
                static_cast<std::size_t>(record.owner_cluster)];
        if (owner.owner_cluster != record.owner_cluster ||
            !same_group(record, owner)) {
            return fail(
                LineFragmentLayoutErrorKind::ClusterMappingViolation,
                0U,
                0U,
                cluster,
                record.segment_index,
                "glyph-cluster continuation does not match its owner group",
                error);
        }
        const ShapedGlyphRun& run =
            request.shaped_text->segments[record.segment_index].glyphs;
        const std::size_t first_glyph = record.first_glyph;
        const std::size_t glyph_count = record.glyph_count;
        if (cluster < run.first_cluster || cluster >= run.cluster_limit ||
            record.owner_cluster < run.first_cluster ||
            record.owner_cluster >= run.cluster_limit ||
            first_glyph > run.glyphs.size() ||
            glyph_count > run.glyphs.size() - first_glyph) {
            return fail(
                LineFragmentLayoutErrorKind::ClusterMappingViolation,
                0U,
                0U,
                cluster,
                record.segment_index,
                "glyph-cluster record references an invalid glyph span",
                error);
        }
    }

    stats->input_lines = request.line_selection->lines.size();
    stats->input_segments = request.shaped_text->segments.size();
    stats->input_clusters = request.cluster_count;
    return true;
}

bool build_active_line_spans(
    const LineFragmentLayoutRequest& request,
    std::pmr::vector<BidiLineSpan>* active_lines,
    LineFragmentLayoutStats* stats,
    LineFragmentLayoutError* error) noexcept {
    const auto& active_indices = request.bidi_topology->active_unit_indices;
    std::size_t active_cursor = 0U;
    try {
        active_lines->reserve(request.line_selection->lines.size());
    } catch (const std::bad_alloc&) {
        return fail(
            LineFragmentLayoutErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            0U,
            "active line-partition working set exceeds its hard budget",
            error);
    } catch (...) {
        return fail(
            LineFragmentLayoutErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            0U,
            "active line-partition allocation failed",
            error);
    }

    for (std::size_t line_index = 0U;
         line_index < request.line_selection->lines.size();
         ++line_index) {
        const std::uint32_t first_cluster =
            first_cluster_for_line(*request.line_selection, line_index);
        const std::uint32_t cluster_limit =
            request.line_selection->lines[line_index].cluster_limit;
        const std::uint32_t first_codepoint = request.cluster_count == 0U
            ? 0U
            : request.grapheme_boundaries[first_cluster].codepoint_index;
        const std::uint32_t codepoint_limit = request.cluster_count == 0U
            ? 0U
            : request.grapheme_boundaries[cluster_limit].codepoint_index;
        const std::size_t first_active = active_cursor;
        while (active_cursor < active_indices.size()) {
            const BidiExplicitUnit& unit = request.bidi_units[
                active_indices[active_cursor]];
            if (unit.codepoint_index < first_codepoint) {
                return fail(
                    LineFragmentLayoutErrorKind::TopologyViolation,
                    line_index,
                    active_cursor,
                    first_cluster,
                    0U,
                    "active bidi unit precedes its selected logical line",
                    error);
            }
            if (unit.codepoint_index >= codepoint_limit) {
                break;
            }
            ++active_cursor;
        }
        const std::size_t active_count = active_cursor - first_active;
        if (active_count == 0U) {
            ++stats->zero_active_lines;
            continue;
        }
        if (first_active > std::numeric_limits<std::uint32_t>::max() ||
            active_count > std::numeric_limits<std::uint32_t>::max()) {
            return fail(
                LineFragmentLayoutErrorKind::OutputBudgetExceeded,
                line_index,
                first_active,
                first_cluster,
                0U,
                "active line span exceeds the compact index domain",
                error);
        }
        try {
            active_lines->push_back(BidiLineSpan{
                static_cast<std::uint32_t>(first_active),
                static_cast<std::uint32_t>(active_count)});
        } catch (const std::bad_alloc&) {
            return fail(
                LineFragmentLayoutErrorKind::OutputBudgetExceeded,
                line_index,
                first_active,
                first_cluster,
                0U,
                "active line-partition working set exceeds its hard budget",
                error);
        } catch (...) {
            return fail(
                LineFragmentLayoutErrorKind::OutputBudgetExceeded,
                line_index,
                first_active,
                first_cluster,
                0U,
                "active line-partition publication failed",
                error);
        }
    }

    if (active_cursor != active_indices.size()) {
        return fail(
            LineFragmentLayoutErrorKind::TopologyViolation,
            request.line_selection->lines.size(),
            active_cursor,
            request.cluster_count,
            0U,
            "active bidi units extend past the selected line partition",
            error);
    }
    return true;
}

bool build_cluster_metadata(
    const LineFragmentLayoutRequest& request,
    const BidiVisualOrder& visual,
    std::pmr::vector<ClusterVisualMetadata>* metadata,
    LineFragmentLayoutStats* stats,
    LineFragmentLayoutError* error) noexcept {
    try {
        metadata->resize(request.cluster_count);
    } catch (const std::bad_alloc&) {
        return fail(
            LineFragmentLayoutErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            0U,
            "cluster visual-metadata working set exceeds its hard budget",
            error);
    } catch (...) {
        return fail(
            LineFragmentLayoutErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            0U,
            "cluster visual-metadata allocation failed",
            error);
    }

    for (std::uint32_t cluster = 0U;
         cluster < request.cluster_count;
         ++cluster) {
        const std::uint32_t segment_index =
            request.cluster_map->records[cluster].segment_index;
        (*metadata)[cluster].level =
            request.shaped_text->segments[segment_index].run.bidi_level;
    }

    std::size_t cluster_cursor = 0U;
    for (std::size_t active = 0U;
         active < request.bidi_topology->active_unit_indices.size();
         ++active) {
        const BidiExplicitUnit& unit = request.bidi_units[
            request.bidi_topology->active_unit_indices[active]];
        while (cluster_cursor < request.cluster_count &&
               unit.codepoint_index >=
                   request.grapheme_boundaries[cluster_cursor + 1U]
                       .codepoint_index) {
            ++cluster_cursor;
        }
        if (cluster_cursor >= request.cluster_count ||
            unit.codepoint_index <
                request.grapheme_boundaries[cluster_cursor].codepoint_index) {
            return fail(
                LineFragmentLayoutErrorKind::ClusterMappingViolation,
                0U,
                active,
                static_cast<std::uint32_t>(cluster_cursor),
                0U,
                "active bidi unit cannot be mapped to one grapheme cluster",
                error);
        }

        ClusterVisualMetadata& cluster = (*metadata)[cluster_cursor];
        const std::uint8_t adjusted_level = visual.line_levels[active];
        if ((cluster.flags & kClusterHasActive) == 0U) {
            cluster.level = adjusted_level;
            cluster.flags |= kClusterHasActive;
        } else if ((cluster.level & 1U) != (adjusted_level & 1U)) {
            return fail(
                LineFragmentLayoutErrorKind::MixedClusterDirection,
                0U,
                active,
                static_cast<std::uint32_t>(cluster_cursor),
                request.cluster_map->records[cluster_cursor].segment_index,
                "one grapheme cluster contains L1-adjusted levels of both parities",
                error);
        } else if (cluster.level != adjusted_level &&
                   (cluster.flags & kClusterMixedLevel) == 0U) {
            cluster.flags |= kClusterMixedLevel;
            ++stats->same_direction_mixed_level_clusters;
        }
        if (adjusted_level != request.implicit_levels[active]) {
            cluster.flags |= kClusterL1Adjusted;
        }
    }

    for (std::size_t cluster_index = 0U;
         cluster_index < metadata->size();
         ++cluster_index) {
        const ClusterVisualMetadata& cluster = (*metadata)[cluster_index];
        if ((cluster.flags & kClusterHasActive) == 0U) {
            ++stats->zero_active_clusters;
        }
        if ((cluster.flags & kClusterL1Adjusted) != 0U) {
            ++stats->l1_adjusted_clusters;
        }
    }
    return true;
}

bool build_advance_prefix(
    const LineFragmentLayoutRequest& request,
    std::pmr::vector<std::uint64_t>* prefix,
    LineFragmentLayoutError* error) noexcept {
    try {
        prefix->resize(static_cast<std::size_t>(request.cluster_count) + 1U, 0U);
    } catch (const std::bad_alloc&) {
        return fail(
            LineFragmentLayoutErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            0U,
            "cluster advance-prefix working set exceeds its hard budget",
            error);
    } catch (...) {
        return fail(
            LineFragmentLayoutErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            0U,
            "cluster advance-prefix allocation failed",
            error);
    }

    for (std::uint32_t cluster = 0U;
         cluster < request.cluster_count;
         ++cluster) {
        const GlyphClusterRecord& record =
            request.cluster_map->records[cluster];
        std::uint64_t advance = 0U;
        if (record.owner_cluster == cluster) {
            const ShapedGlyphRun& run =
                request.shaped_text->segments[record.segment_index].glyphs;
            std::int64_t signed_advance = 0;
            for (std::size_t offset = 0U;
                 offset < record.glyph_count;
                 ++offset) {
                const std::size_t glyph_index =
                    static_cast<std::size_t>(record.first_glyph) + offset;
                if (!checked_add_signed(
                        &signed_advance,
                        run.glyphs[glyph_index].x_advance)) {
                    return fail(
                        LineFragmentLayoutErrorKind::AdvanceOverflow,
                        0U,
                        0U,
                        cluster,
                        record.segment_index,
                        "signed glyph-group inline advance overflowed",
                        error);
                }
            }
            advance = advance_magnitude(signed_advance);
        }
        (*prefix)[static_cast<std::size_t>(cluster) + 1U] =
            (*prefix)[cluster];
        if (!checked_add(
                &(*prefix)[static_cast<std::size_t>(cluster) + 1U],
                advance)) {
            return fail(
                LineFragmentLayoutErrorKind::AdvanceOverflow,
                0U,
                0U,
                cluster,
                record.segment_index,
                "cluster advance prefix overflowed",
                error);
        }
    }
    return true;
}

bool fragment_split_before(
    const LineFragmentLayoutRequest& request,
    const std::pmr::vector<ClusterVisualMetadata>& metadata,
    std::uint32_t cluster) noexcept {
    const GlyphClusterRecord& previous =
        request.cluster_map->records[cluster - 1U];
    const GlyphClusterRecord& current =
        request.cluster_map->records[cluster];
    return previous.segment_index != current.segment_index ||
           metadata[cluster - 1U].level != metadata[cluster].level;
}

bool count_fragments(
    const LineFragmentLayoutRequest& request,
    const std::pmr::vector<ClusterVisualMetadata>& metadata,
    std::uint64_t* fragment_count,
    LineFragmentLayoutError* error) noexcept {
    *fragment_count = 0U;
    for (std::size_t line_index = 0U;
         line_index < request.line_selection->lines.size();
         ++line_index) {
        const std::uint32_t first =
            first_cluster_for_line(*request.line_selection, line_index);
        const std::uint32_t limit =
            request.line_selection->lines[line_index].cluster_limit;
        if (first == limit) {
            continue;
        }
        if (!checked_add(fragment_count, 1U)) {
            return fail(
                LineFragmentLayoutErrorKind::AggregateOverflow,
                line_index,
                0U,
                first,
                request.cluster_map->records[first].segment_index,
                "fragment count overflowed",
                error);
        }
        for (std::uint32_t cluster = first + 1U;
             cluster < limit;
             ++cluster) {
            if (!fragment_split_before(request, metadata, cluster)) {
                continue;
            }
            if (!is_safe_cluster_boundary(
                    *request.cluster_map,
                    cluster,
                    request.cluster_count)) {
                return fail(
                    LineFragmentLayoutErrorKind::UnsafeFragmentBoundary,
                    line_index,
                    0U,
                    cluster,
                    request.cluster_map->records[cluster].segment_index,
                    "visual fragment split crosses one merged glyph group",
                    error);
            }
            if (!checked_add(fragment_count, 1U)) {
                return fail(
                    LineFragmentLayoutErrorKind::AggregateOverflow,
                    line_index,
                    0U,
                    cluster,
                    request.cluster_map->records[cluster].segment_index,
                    "fragment count overflowed",
                    error);
            }
        }
    }
    return true;
}

void reverse_fragment_span(
    InlineLayoutFragment* first,
    InlineLayoutFragment* last,
    LineFragmentLayoutStats* stats) noexcept {
    const std::size_t count = static_cast<std::size_t>(last - first);
    if (count <= 1U) {
        return;
    }
    std::reverse(first, last);
    ++stats->l2_reversal_spans;
    stats->l2_reversed_fragments += count;
}

void apply_fragment_l2(
    InlineLayoutFragment* fragments,
    std::size_t fragment_count,
    LineFragmentLayoutStats* stats) noexcept {
    if (fragment_count == 0U) {
        return;
    }
    std::uint8_t maximum = 0U;
    std::uint8_t lowest_odd = 127U;
    for (std::size_t index = 0U; index < fragment_count; ++index) {
        const std::uint8_t level = fragments[index].bidi_level;
        maximum = std::max(maximum, level);
        if ((level & 1U) != 0U) {
            lowest_odd = std::min(lowest_odd, level);
        }
    }
    if (lowest_odd == 127U) {
        return;
    }

    for (int threshold = static_cast<int>(maximum);
         threshold >= static_cast<int>(lowest_odd);
         --threshold) {
        std::size_t position = 0U;
        while (position < fragment_count) {
            while (position < fragment_count &&
                   fragments[position].bidi_level < threshold) {
                ++position;
            }
            const std::size_t first = position;
            while (position < fragment_count &&
                   fragments[position].bidi_level >= threshold) {
                ++position;
            }
            reverse_fragment_span(
                fragments + first,
                fragments + position,
                stats);
        }
    }
}

bool fill_layout(
    const LineFragmentLayoutRequest& request,
    const std::pmr::vector<ClusterVisualMetadata>& metadata,
    const std::pmr::vector<std::uint64_t>& prefix,
    LineFragmentLayout* working,
    LineFragmentLayoutStats* stats,
    LineFragmentLayoutError* error) noexcept {
    std::size_t output_fragment = 0U;
    for (std::size_t line_index = 0U;
         line_index < request.line_selection->lines.size();
         ++line_index) {
        const SelectedLineRecord& selected =
            request.line_selection->lines[line_index];
        const std::uint32_t first =
            first_cluster_for_line(*request.line_selection, line_index);
        const std::uint32_t limit = selected.cluster_limit;
        const std::size_t line_first_fragment = output_fragment;

        std::uint32_t fragment_start = first;
        while (fragment_start < limit) {
            std::uint32_t fragment_limit = fragment_start + 1U;
            while (fragment_limit < limit &&
                   !fragment_split_before(request, metadata, fragment_limit)) {
                ++fragment_limit;
            }

            const std::uint32_t segment_index =
                request.cluster_map->records[fragment_start].segment_index;
            const MultiRunShapedSegment& segment =
                request.shaped_text->segments[segment_index];
            std::uint8_t flags = segment.run.direction ==
                    ShapingDirection::RightToLeft
                ? static_cast<std::uint8_t>(kInlineFragmentGlyphRunRtl)
                : static_cast<std::uint8_t>(0U);
            bool l1_adjusted = false;
            bool contains_x9_only = false;
            for (std::uint32_t cluster = fragment_start;
                 cluster < fragment_limit;
                 ++cluster) {
                l1_adjusted = l1_adjusted ||
                    (metadata[cluster].flags & kClusterL1Adjusted) != 0U;
                contains_x9_only = contains_x9_only ||
                    (metadata[cluster].flags & kClusterHasActive) == 0U;
            }
            if (l1_adjusted) {
                flags |= kInlineFragmentL1Adjusted;
            }
            if (contains_x9_only) {
                flags |= kInlineFragmentContainsX9Only;
            }

            working->fragments[output_fragment] = InlineLayoutFragment{
                0U,
                prefix[fragment_limit] - prefix[fragment_start],
                segment_index,
                fragment_start,
                fragment_limit,
                metadata[fragment_start].level,
                flags,
                0U};
            ++output_fragment;
            fragment_start = fragment_limit;
        }

        const std::size_t fragment_count =
            output_fragment - line_first_fragment;
        if (fragment_count != 0U) {
            apply_fragment_l2(
                working->fragments.data() + line_first_fragment,
                fragment_count,
                stats);
        }

        std::uint64_t inline_offset = 0U;
        std::uint32_t line_flags = selected.flags;
        for (std::size_t index = line_first_fragment;
             index < output_fragment;
             ++index) {
            InlineLayoutFragment& fragment = working->fragments[index];
            fragment.inline_offset = inline_offset;
            if (!checked_add(&inline_offset, fragment.inline_advance)) {
                return fail(
                    LineFragmentLayoutErrorKind::AdvanceOverflow,
                    line_index,
                    0U,
                    fragment.first_cluster,
                    fragment.segment_index,
                    "visual fragment inline offsets overflowed",
                    error);
            }
            ++stats->output_fragments;
            stats->maximum_fragment_level = std::max(
                stats->maximum_fragment_level,
                fragment.bidi_level);
            if ((fragment.bidi_level & 1U) != 0U ||
                (fragment.flags & kInlineFragmentGlyphRunRtl) != 0U) {
                line_flags |= kVisualLineContainsRtl;
            }
            if ((fragment.flags & kInlineFragmentGlyphRunRtl) != 0U) {
                ++stats->rtl_fragments;
            }
            if ((fragment.flags & kInlineFragmentL1Adjusted) != 0U) {
                line_flags |= kVisualLineL1Adjusted;
                ++stats->l1_adjusted_fragments;
            }
            if ((fragment.flags & kInlineFragmentContainsX9Only) != 0U) {
                line_flags |= kVisualLineContainsX9Only;
                ++stats->x9_only_fragments;
            }
        }

        const std::uint64_t prefix_line_advance = prefix[limit] - prefix[first];
        if (inline_offset != selected.inline_advance ||
            prefix_line_advance != selected.inline_advance) {
            return fail(
                LineFragmentLayoutErrorKind::TopologyViolation,
                line_index,
                0U,
                first,
                fragment_count == 0U
                    ? 0U
                    : working->fragments[line_first_fragment].segment_index,
                "fragment advances do not reproduce the selected-line advance",
                error);
        }
        if (line_first_fragment > std::numeric_limits<std::uint32_t>::max() ||
            fragment_count > std::numeric_limits<std::uint32_t>::max()) {
            return fail(
                LineFragmentLayoutErrorKind::OutputBudgetExceeded,
                line_index,
                0U,
                first,
                0U,
                "line fragment slice exceeds the compact index domain",
                error);
        }

        working->lines[line_index] = VisualLineLayoutRecord{
            selected.inline_advance,
            static_cast<std::uint32_t>(line_first_fragment),
            static_cast<std::uint32_t>(fragment_count),
            limit,
            line_flags};
        ++stats->output_lines;
        if (!checked_add(
                &stats->total_inline_advance,
                selected.inline_advance)) {
            return fail(
                LineFragmentLayoutErrorKind::AggregateOverflow,
                line_index,
                0U,
                first,
                0U,
                "layout inline-advance aggregate overflowed",
                error);
        }
        stats->maximum_line_advance = std::max(
            stats->maximum_line_advance,
            selected.inline_advance);
        stats->maximum_fragments_per_line = std::max(
            stats->maximum_fragments_per_line,
            static_cast<std::uint64_t>(fragment_count));
    }

    return output_fragment == working->fragments.size() || fail(
        LineFragmentLayoutErrorKind::TopologyViolation,
        request.line_selection->lines.size(),
        0U,
        request.cluster_count,
        0U,
        "fragment counting and publication passes disagree",
        error);
}

} // namespace

LineFragmentLayout::LineFragmentLayout(std::pmr::memory_resource* resource)
    : lines(usable_resource(resource)),
      fragments(usable_resource(resource)) {}

std::pmr::memory_resource* LineFragmentLayout::resource() const noexcept {
    return lines.get_allocator().resource();
}

void LineFragmentLayout::release() noexcept {
    release_vector(&lines);
    release_vector(&fragments);
}

const char* line_fragment_layout_error_kind_name(
    LineFragmentLayoutErrorKind kind) noexcept {
    switch (kind) {
        case LineFragmentLayoutErrorKind::None:
            return "none";
        case LineFragmentLayoutErrorKind::InvalidInput:
            return "invalid_input";
        case LineFragmentLayoutErrorKind::TopologyViolation:
            return "topology_violation";
        case LineFragmentLayoutErrorKind::ClusterMappingViolation:
            return "cluster_mapping_violation";
        case LineFragmentLayoutErrorKind::MixedClusterDirection:
            return "mixed_cluster_direction";
        case LineFragmentLayoutErrorKind::UnsafeFragmentBoundary:
            return "unsafe_fragment_boundary";
        case LineFragmentLayoutErrorKind::BidiVisualFailure:
            return "bidi_visual_failure";
        case LineFragmentLayoutErrorKind::AdvanceOverflow:
            return "advance_overflow";
        case LineFragmentLayoutErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
        case LineFragmentLayoutErrorKind::AggregateOverflow:
            return "aggregate_overflow";
    }
    return "invalid";
}

bool build_line_fragment_layout(
    const LineFragmentLayoutRequest& request,
    LineFragmentLayout* output,
    LineFragmentLayoutStats* stats,
    LineFragmentLayoutError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    output->release();
    *stats = {};
    clear_error(error);

    if (request.bidi_topology == nullptr ||
        request.shaped_text == nullptr ||
        request.cluster_map == nullptr ||
        request.line_selection == nullptr ||
        request.cluster_count == std::numeric_limits<std::uint32_t>::max()) {
        return fail(
            LineFragmentLayoutErrorKind::InvalidInput,
            0U,
            0U,
            0U,
            0U,
            "line-fragment layout requires bounded grapheme, bidi, shaping, cluster, and line inputs",
            error);
    }

    if (!validate_grapheme_and_bidi_inputs(request, stats, error) ||
        !validate_shaping_and_lines(request, stats, error)) {
        return false;
    }

    std::pmr::vector<BidiLineSpan> active_lines(output->resource());
    if (!build_active_line_spans(
            request,
            &active_lines,
            stats,
            error)) {
        return false;
    }

    BidiVisualOrder visual(output->resource());
    if (!request.bidi_topology->active_unit_indices.empty()) {
        BidiVisualError bidi_error;
        if (!resolve_bidi_visual_order(
                request.bidi_units,
                *request.bidi_topology,
                request.implicit_levels,
                request.paragraph_level,
                active_lines,
                &visual,
                &stats->bidi_visual,
                &bidi_error)) {
            return fail_bidi(bidi_error, error);
        }
    }

    std::pmr::vector<ClusterVisualMetadata> metadata(output->resource());
    if (!build_cluster_metadata(request, visual, &metadata, stats, error)) {
        return false;
    }

    std::pmr::vector<std::uint64_t> advance_prefix(output->resource());
    if (!build_advance_prefix(request, &advance_prefix, error)) {
        return false;
    }

    std::uint64_t fragment_count = 0U;
    if (!count_fragments(request, metadata, &fragment_count, error) ||
        fragment_count > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max())) {
        return fragment_count <= static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max())
            ? false
            : fail(
                LineFragmentLayoutErrorKind::OutputBudgetExceeded,
                0U,
                0U,
                0U,
                0U,
                "fragment output exceeds the addressable allocation domain",
                error);
    }

    LineFragmentLayout working(output->resource());
    try {
        working.lines.resize(request.line_selection->lines.size());
        working.fragments.resize(static_cast<std::size_t>(fragment_count));
    } catch (const std::bad_alloc&) {
        return fail(
            LineFragmentLayoutErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            0U,
            "line-fragment output exceeds its hard budget",
            error);
    } catch (...) {
        return fail(
            LineFragmentLayoutErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            0U,
            "line-fragment output allocation failed",
            error);
    }

    if (!fill_layout(
            request,
            metadata,
            advance_prefix,
            &working,
            stats,
            error)) {
        return false;
    }

    working.lines.swap(output->lines);
    working.fragments.swap(output->fragments);
    return true;
}

} // namespace zevryon::text
