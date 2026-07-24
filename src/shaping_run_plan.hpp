#pragma once

#include "bidi_explicit.hpp"
#include "bidi_sequence.hpp"
#include "font_fallback.hpp"
#include "harfbuzz_shaper.hpp"
#include "script_run.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

struct ShapingRunBoundary {
    std::uint32_t cluster_index{0};
    FontFaceId face_id{kInvalidFontFaceId};
    ScriptId script{ScriptId::Zzzz};
    ShapingDirection direction{ShapingDirection::LeftToRight};
    FontFallbackSource fallback_source{FontFallbackSource::Missing};
    std::uint8_t bidi_level{0};
    std::uint8_t reserved{0};

    bool operator==(const ShapingRunBoundary&) const noexcept = default;
};

static_assert(
    sizeof(ShapingRunBoundary) <= 16U,
    "shaping-run boundaries must remain within the Z2 memory contract");

enum class ShapingRunPlanErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    TopologyViolation,
    MixedClusterDirection,
    IndexOverflow,
    OutputBudgetExceeded
};

struct ShapingRunPlanError {
    ShapingRunPlanErrorKind kind{ShapingRunPlanErrorKind::None};
    std::size_t cluster_index{0};
    std::string message;
};

struct ShapingRunPlanStats {
    std::uint64_t input_codepoints{0};
    std::uint64_t input_clusters{0};
    std::uint64_t active_bidi_units{0};
    std::uint64_t output_runs{0};
    std::uint64_t left_to_right_runs{0};
    std::uint64_t right_to_left_runs{0};
    std::uint64_t missing_font_runs{0};
    std::uint64_t inherited_direction_clusters{0};
    std::uint64_t same_direction_mixed_level_clusters{0};
    std::uint64_t script_splits{0};
    std::uint64_t font_splits{0};
    std::uint64_t direction_splits{0};
};

class ShapingRunPlan final {
public:
    explicit ShapingRunPlan(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    ShapingRunPlan(const ShapingRunPlan&) = delete;
    ShapingRunPlan& operator=(const ShapingRunPlan&) = delete;
    ShapingRunPlan(ShapingRunPlan&&) noexcept = default;
    ShapingRunPlan& operator=(ShapingRunPlan&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    // Non-empty input produces one boundary for every shaping run plus a final
    // sentinel at cluster_count. Runs never split a grapheme cluster. Missing
    // font runs retain kInvalidFontFaceId and are not silently substituted.
    std::pmr::vector<ShapingRunBoundary> boundaries;
};

const char* shaping_run_plan_error_kind_name(
    ShapingRunPlanErrorKind kind) noexcept;

// Intersects grapheme-atomic Script and font-fallback runs with final bidi-level
// parity. Bidi inputs remain in logical order; this stage does not apply visual
// reordering and does not shape glyphs. X9-removed-only clusters inherit the
// preceding cluster direction, or paragraph_level when no preceding direction
// exists. A cluster containing active scalars of both parities is rejected.
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
    ShapingRunPlanError* error) noexcept;

} // namespace zevryon::text
