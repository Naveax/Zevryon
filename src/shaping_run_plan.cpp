#include "shaping_run_plan.hpp"

#include <limits>
#include <new>
#include <utility>

namespace zevryon::text {
namespace {

struct RunKey {
    FontFaceId face_id{kInvalidFontFaceId};
    ScriptId script{ScriptId::Zzzz};
    ShapingDirection direction{ShapingDirection::LeftToRight};
    FontFallbackSource fallback_source{FontFallbackSource::Missing};
    std::uint8_t bidi_level{0};

    bool operator==(const RunKey&) const noexcept = default;
};

void clear_error(ShapingRunPlanError* error) noexcept {
    if (error == nullptr) {
        return;
    }
    error->kind = ShapingRunPlanErrorKind::None;
    error->cluster_index = 0U;
    error->message.clear();
}

bool fail(
    ShapingRunPlanErrorKind kind,
    std::size_t cluster_index,
    const char* message,
    ShapingRunPlanError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->cluster_index = cluster_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool valid_fallback_source(FontFallbackSource source) noexcept {
    return static_cast<std::uint8_t>(source) <=
           static_cast<std::uint8_t>(FontFallbackSource::Missing);
}

bool validate_graphemes(
    std::span<const DecodedCodePoint> codepoints,
    std::span<const GraphemeBoundary> boundaries,
    ShapingRunPlanError* error) noexcept {
    if (codepoints.empty()) {
        return boundaries.empty() || fail(
            ShapingRunPlanErrorKind::InvalidInput,
            0U,
            "empty codepoint input requires an empty grapheme boundary list",
            error);
    }
    if (boundaries.size() < 2U || boundaries.front().codepoint_index != 0U ||
        boundaries.back().codepoint_index != codepoints.size()) {
        return fail(
            ShapingRunPlanErrorKind::InvalidInput,
            0U,
            "grapheme boundaries must cover the complete codepoint stream",
            error);
    }
    for (std::size_t index = 1U; index < boundaries.size(); ++index) {
        if (boundaries[index - 1U].codepoint_index >=
                boundaries[index].codepoint_index ||
            boundaries[index - 1U].source_offset > boundaries[index].source_offset) {
            return fail(
                ShapingRunPlanErrorKind::InvalidInput,
                index - 1U,
                "grapheme boundaries are not strictly ordered",
                error);
        }
    }
    return true;
}

bool validate_script_boundaries(
    std::span<const ScriptRunBoundary> boundaries,
    std::size_t cluster_count,
    ShapingRunPlanError* error) noexcept {
    if (cluster_count == 0U) {
        return boundaries.empty() || fail(
            ShapingRunPlanErrorKind::InvalidInput,
            0U,
            "empty input requires an empty script-run boundary list",
            error);
    }
    if (boundaries.size() < 2U || boundaries.front().cluster_index != 0U ||
        boundaries.back().cluster_index != cluster_count) {
        return fail(
            ShapingRunPlanErrorKind::InvalidInput,
            0U,
            "script-run boundaries must cover every grapheme cluster",
            error);
    }
    for (std::size_t index = 1U; index < boundaries.size(); ++index) {
        if (boundaries[index - 1U].cluster_index >= boundaries[index].cluster_index) {
            return fail(
                ShapingRunPlanErrorKind::InvalidInput,
                boundaries[index - 1U].cluster_index,
                "script-run boundaries are not strictly increasing",
                error);
        }
    }
    return true;
}

bool validate_fallback_boundaries(
    std::span<const FontFallbackBoundary> boundaries,
    std::size_t cluster_count,
    ShapingRunPlanError* error) noexcept {
    if (cluster_count == 0U) {
        return boundaries.empty() || fail(
            ShapingRunPlanErrorKind::InvalidInput,
            0U,
            "empty input requires an empty fallback boundary list",
            error);
    }
    if (boundaries.size() < 2U || boundaries.front().cluster_index != 0U ||
        boundaries.back().cluster_index != cluster_count) {
        return fail(
            ShapingRunPlanErrorKind::InvalidInput,
            0U,
            "fallback boundaries must cover every grapheme cluster",
            error);
    }
    for (std::size_t index = 0U; index + 1U < boundaries.size(); ++index) {
        const FontFallbackBoundary& boundary = boundaries[index];
        if (!valid_fallback_source(boundary.source) ||
            ((boundary.source == FontFallbackSource::Missing) !=
             (boundary.face_id == kInvalidFontFaceId))) {
            return fail(
                ShapingRunPlanErrorKind::InvalidInput,
                boundary.cluster_index,
                "fallback face and source classification disagree",
                error);
        }
        if (boundary.cluster_index >= boundaries[index + 1U].cluster_index) {
            return fail(
                ShapingRunPlanErrorKind::InvalidInput,
                boundary.cluster_index,
                "fallback boundaries are not strictly increasing",
                error);
        }
    }
    return true;
}

bool validate_bidi_topology(
    std::span<const DecodedCodePoint> codepoints,
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    std::span<const std::uint8_t> final_levels,
    ShapingRunPlanError* error) noexcept {
    if (topology.active_unit_indices.size() != final_levels.size()) {
        return fail(
            ShapingRunPlanErrorKind::TopologyViolation,
            0U,
            "final bidi levels do not match the X9-active stream",
            error);
    }
    std::uint32_t previous_unit = 0U;
    std::uint32_t previous_codepoint = 0U;
    bool have_previous = false;
    for (std::size_t active = 0U; active < topology.active_unit_indices.size(); ++active) {
        const std::uint32_t unit_index = topology.active_unit_indices[active];
        if (unit_index >= units.size()) {
            return fail(
                ShapingRunPlanErrorKind::TopologyViolation,
                active,
                "active bidi index is outside the explicit-unit stream",
                error);
        }
        const BidiExplicitUnit& unit = units[unit_index];
        if ((unit.flags & kBidiUnitRemovedByX9) != 0U ||
            unit.codepoint_index >= codepoints.size() || final_levels[active] > 126U ||
            (have_previous &&
             (unit_index <= previous_unit || unit.codepoint_index <= previous_codepoint))) {
            return fail(
                ShapingRunPlanErrorKind::TopologyViolation,
                active,
                "X9-active bidi topology is invalid or not in logical order",
                error);
        }
        previous_unit = unit_index;
        previous_codepoint = unit.codepoint_index;
        have_previous = true;
    }
    return true;
}

struct WalkResult {
    std::size_t run_count{0};
    ShapingRunPlanStats stats{};
};

template <typename Emit>
bool walk_plan(
    std::span<const GraphemeBoundary> grapheme_boundaries,
    std::span<const ScriptRunBoundary> script_boundaries,
    std::span<const FontFallbackBoundary> fallback_boundaries,
    std::span<const BidiExplicitUnit> bidi_units,
    const BidiSequenceTopology& topology,
    std::span<const std::uint8_t> final_levels,
    std::uint8_t paragraph_level,
    bool collect_stats,
    WalkResult* result,
    Emit&& emit,
    ShapingRunPlanError* error) noexcept {
    const std::size_t cluster_count = grapheme_boundaries.size() - 1U;
    std::size_t script_index = 0U;
    std::size_t fallback_index = 0U;
    std::size_t active_index = 0U;
    std::uint8_t inherited_level = paragraph_level;
    RunKey previous{};
    bool have_previous = false;

    for (std::size_t cluster = 0U; cluster < cluster_count; ++cluster) {
        while (script_index + 1U < script_boundaries.size() &&
               script_boundaries[script_index + 1U].cluster_index == cluster) {
            ++script_index;
        }
        while (fallback_index + 1U < fallback_boundaries.size() &&
               fallback_boundaries[fallback_index + 1U].cluster_index == cluster) {
            ++fallback_index;
        }

        const std::uint32_t first_codepoint =
            grapheme_boundaries[cluster].codepoint_index;
        const std::uint32_t codepoint_limit =
            grapheme_boundaries[cluster + 1U].codepoint_index;
        bool have_active = false;
        bool mixed_level_same_direction = false;
        std::uint8_t cluster_level = inherited_level;

        while (active_index < topology.active_unit_indices.size()) {
            const BidiExplicitUnit& unit =
                bidi_units[topology.active_unit_indices[active_index]];
            if (unit.codepoint_index < first_codepoint) {
                return fail(
                    ShapingRunPlanErrorKind::TopologyViolation,
                    cluster,
                    "active bidi unit precedes its grapheme cluster",
                    error);
            }
            if (unit.codepoint_index >= codepoint_limit) {
                break;
            }
            const std::uint8_t level = final_levels[active_index];
            if (!have_active) {
                cluster_level = level;
                have_active = true;
            } else if ((cluster_level & 1U) != (level & 1U)) {
                return fail(
                    ShapingRunPlanErrorKind::MixedClusterDirection,
                    cluster,
                    "one grapheme cluster contains active bidi levels of both parities",
                    error);
            } else if (cluster_level != level) {
                mixed_level_same_direction = true;
            }
            ++active_index;
        }

        if (have_active) {
            inherited_level = cluster_level;
        } else if (collect_stats) {
            ++result->stats.inherited_direction_clusters;
        }
        if (mixed_level_same_direction && collect_stats) {
            ++result->stats.same_direction_mixed_level_clusters;
        }

        const FontFallbackBoundary& fallback = fallback_boundaries[fallback_index];
        const RunKey current{
            fallback.face_id,
            script_boundaries[script_index].script,
            (cluster_level & 1U) == 0U ? ShapingDirection::LeftToRight
                                       : ShapingDirection::RightToLeft,
            fallback.source,
            cluster_level};

        if (!have_previous || !(current == previous)) {
            if (collect_stats) {
                ++result->stats.output_runs;
                if (current.direction == ShapingDirection::LeftToRight) {
                    ++result->stats.left_to_right_runs;
                } else {
                    ++result->stats.right_to_left_runs;
                }
                if (current.face_id == kInvalidFontFaceId) {
                    ++result->stats.missing_font_runs;
                }
                if (have_previous) {
                    if (current.script != previous.script) {
                        ++result->stats.script_splits;
                    }
                    if (current.face_id != previous.face_id ||
                        current.fallback_source != previous.fallback_source) {
                        ++result->stats.font_splits;
                    }
                    if (current.direction != previous.direction) {
                        ++result->stats.direction_splits;
                    }
                    if (current.bidi_level != previous.bidi_level) {
                        ++result->stats.level_splits;
                    }
                }
            }
            ++result->run_count;
            emit(static_cast<std::uint32_t>(cluster), current);
            previous = current;
            have_previous = true;
        }
    }

    if (active_index != topology.active_unit_indices.size()) {
        return fail(
            ShapingRunPlanErrorKind::TopologyViolation,
            cluster_count,
            "active bidi units extend past the grapheme stream",
            error);
    }
    return true;
}

} // namespace

ShapingRunPlan::ShapingRunPlan(std::pmr::memory_resource* resource)
    : boundaries(resource) {}

std::pmr::memory_resource* ShapingRunPlan::resource() const noexcept {
    return boundaries.get_allocator().resource();
}

void ShapingRunPlan::release() noexcept {
    std::pmr::vector<ShapingRunBoundary> empty(resource());
    boundaries.swap(empty);
}

const char* shaping_run_plan_error_kind_name(
    ShapingRunPlanErrorKind kind) noexcept {
    switch (kind) {
        case ShapingRunPlanErrorKind::None:
            return "none";
        case ShapingRunPlanErrorKind::InvalidInput:
            return "invalid_input";
        case ShapingRunPlanErrorKind::TopologyViolation:
            return "topology_violation";
        case ShapingRunPlanErrorKind::MixedClusterDirection:
            return "mixed_cluster_direction";
        case ShapingRunPlanErrorKind::IndexOverflow:
            return "index_overflow";
        case ShapingRunPlanErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
    }
    return "invalid";
}

bool build_shaping_run_plan(
    std::span<const DecodedCodePoint> codepoints,
    std::span<const GraphemeBoundary> grapheme_boundaries,
    std::span<const ScriptRunBoundary> script_run_boundaries,
    std::span<const FontFallbackBoundary> fallback_boundaries,
    std::span<const BidiExplicitUnit> bidi_units,
    const BidiSequenceTopology& bidi_topology,
    std::span<const std::uint8_t> final_bidi_levels,
    std::uint8_t paragraph_level,
    ShapingRunPlan* output,
    ShapingRunPlanStats* stats,
    ShapingRunPlanError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    output->release();
    *stats = {};
    clear_error(error);

    if (paragraph_level > 1U ||
        codepoints.size() > static_cast<std::size_t>(
                                std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            codepoints.size() > static_cast<std::size_t>(
                                    std::numeric_limits<std::uint32_t>::max())
                ? ShapingRunPlanErrorKind::IndexOverflow
                : ShapingRunPlanErrorKind::InvalidInput,
            0U,
            "paragraph level or codepoint count is outside the shaping-plan contract",
            error);
    }
    if (!validate_graphemes(codepoints, grapheme_boundaries, error)) {
        return false;
    }
    if (codepoints.empty()) {
        if (!script_run_boundaries.empty() || !fallback_boundaries.empty() ||
            !bidi_units.empty() || !bidi_topology.active_unit_indices.empty() ||
            !final_bidi_levels.empty()) {
            return fail(
                ShapingRunPlanErrorKind::InvalidInput,
                0U,
                "empty shaping input requires empty script, fallback, and bidi inputs",
                error);
        }
        return true;
    }

    const std::size_t cluster_count = grapheme_boundaries.size() - 1U;
    if (cluster_count > static_cast<std::size_t>(
                            std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            ShapingRunPlanErrorKind::IndexOverflow,
            cluster_count,
            "grapheme cluster count exceeds the 32-bit shaping-run contract",
            error);
    }
    if (!validate_script_boundaries(
            script_run_boundaries, cluster_count, error) ||
        !validate_fallback_boundaries(
            fallback_boundaries, cluster_count, error) ||
        !validate_bidi_topology(
            codepoints,
            bidi_units,
            bidi_topology,
            final_bidi_levels,
            error)) {
        return false;
    }

    WalkResult counted{};
    counted.stats.input_codepoints = codepoints.size();
    counted.stats.input_clusters = cluster_count;
    counted.stats.active_bidi_units = bidi_topology.active_unit_indices.size();
    const auto count_only = [](std::uint32_t, const RunKey&) noexcept {};
    if (!walk_plan(
            grapheme_boundaries,
            script_run_boundaries,
            fallback_boundaries,
            bidi_units,
            bidi_topology,
            final_bidi_levels,
            paragraph_level,
            true,
            &counted,
            count_only,
            error)) {
        return false;
    }

    if (counted.run_count == std::numeric_limits<std::size_t>::max()) {
        return fail(
            ShapingRunPlanErrorKind::IndexOverflow,
            cluster_count,
            "shaping-run sentinel count overflows size_t",
            error);
    }

    try {
        output->boundaries.reserve(counted.run_count + 1U);
        WalkResult emitted{};
        const auto publish = [&output](
                                 std::uint32_t cluster_index,
                                 const RunKey& key) {
            output->boundaries.push_back(ShapingRunBoundary{
                cluster_index,
                key.face_id,
                key.script,
                key.direction,
                key.fallback_source,
                key.bidi_level,
                0U});
        };
        if (!walk_plan(
                grapheme_boundaries,
                script_run_boundaries,
                fallback_boundaries,
                bidi_units,
                bidi_topology,
                final_bidi_levels,
                paragraph_level,
                false,
                &emitted,
                publish,
                error)) {
            output->release();
            return false;
        }
        const ShapingDirection sentinel_direction =
            counted.run_count == 0U
                ? ((paragraph_level & 1U) == 0U
                       ? ShapingDirection::LeftToRight
                       : ShapingDirection::RightToLeft)
                : output->boundaries.back().direction;
        const std::uint8_t sentinel_level =
            counted.run_count == 0U ? paragraph_level
                                    : output->boundaries.back().bidi_level;
        output->boundaries.push_back(ShapingRunBoundary{
            static_cast<std::uint32_t>(cluster_count),
            kInvalidFontFaceId,
            ScriptId::Zzzz,
            sentinel_direction,
            FontFallbackSource::Missing,
            sentinel_level,
            0U});
    } catch (const std::bad_alloc&) {
        output->release();
        return fail(
            ShapingRunPlanErrorKind::OutputBudgetExceeded,
            cluster_count,
            "shaping-run output exceeded its resource budget",
            error);
    } catch (...) {
        output->release();
        return fail(
            ShapingRunPlanErrorKind::OutputBudgetExceeded,
            cluster_count,
            "shaping-run output allocation failed",
            error);
    }

    *stats = counted.stats;
    return true;
}

} // namespace zevryon::text
