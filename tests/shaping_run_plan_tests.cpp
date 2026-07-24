#include "ledger_memory_resource.hpp"
#include "shaping_run_plan.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <string>
#include <vector>

namespace {

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

std::vector<zevryon::text::DecodedCodePoint> make_codepoints(
    std::size_t count) {
    std::vector<zevryon::text::DecodedCodePoint> output;
    output.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        output.emplace_back(
            static_cast<std::uint32_t>('a' + (index % 20U)),
            index,
            index + 1U,
            false);
    }
    return output;
}

std::vector<zevryon::text::GraphemeBoundary> make_singleton_graphemes(
    std::size_t count) {
    std::vector<zevryon::text::GraphemeBoundary> output;
    output.reserve(count + 1U);
    for (std::size_t index = 0U; index <= count; ++index) {
        output.push_back(zevryon::text::GraphemeBoundary{
            index,
            static_cast<std::uint32_t>(index)});
    }
    return output;
}

std::vector<zevryon::text::BidiExplicitUnit> make_units(
    std::size_t count) {
    std::vector<zevryon::text::BidiExplicitUnit> output;
    output.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        output.push_back(zevryon::text::BidiExplicitUnit{
            index,
            static_cast<std::uint32_t>(index),
            zevryon::text::BidiClass::L,
            zevryon::text::BidiClass::L,
            0U,
            0U});
    }
    return output;
}

bool test_intersection_and_sentinel() {
    using namespace zevryon::text;

    const auto codepoints = make_codepoints(8U);
    const auto graphemes = make_singleton_graphemes(8U);
    const std::array scripts{
        ScriptRunBoundary{0U, 0U, ScriptId::Latn, 0U},
        ScriptRunBoundary{3U, 3U, ScriptId::Arab, 0U},
        ScriptRunBoundary{6U, 6U, ScriptId::Latn, 0U},
        ScriptRunBoundary{8U, 8U, ScriptId::Zzzz, 0U}};
    const std::array fallback{
        FontFallbackBoundary{0U, 10U, FontFallbackSource::Primary, 0U, 0U, 0U},
        FontFallbackBoundary{2U, 11U, FontFallbackSource::ScriptMatch, 0U, 0U, 0U},
        FontFallbackBoundary{5U, kInvalidFontFaceId, FontFallbackSource::Missing, 0U, 0U, 0U},
        FontFallbackBoundary{7U, 10U, FontFallbackSource::Primary, 0U, 0U, 0U},
        FontFallbackBoundary{8U, kInvalidFontFaceId, FontFallbackSource::Missing, 0U, 0U, 0U}};
    const auto units = make_units(8U);
    BidiSequenceTopology topology;
    for (std::uint32_t index = 0U; index < 8U; ++index) {
        topology.active_unit_indices.push_back(index);
    }
    const std::array<std::uint8_t, 8U> levels{0U, 0U, 0U, 1U, 1U, 1U, 0U, 0U};

    ShapingRunPlan plan;
    ShapingRunPlanStats stats;
    ShapingRunPlanError error;
    if (!require(
            build_shaping_run_plan(
                codepoints,
                graphemes,
                scripts,
                fallback,
                units,
                topology,
                levels,
                0U,
                &plan,
                &stats,
                &error),
            "intersection fixture builds")) {
        return false;
    }

    const std::array expected{
        ShapingRunBoundary{0U, 10U, ScriptId::Latn, ShapingDirection::LeftToRight, FontFallbackSource::Primary, 0U, 0U},
        ShapingRunBoundary{2U, 11U, ScriptId::Latn, ShapingDirection::LeftToRight, FontFallbackSource::ScriptMatch, 0U, 0U},
        ShapingRunBoundary{3U, 11U, ScriptId::Arab, ShapingDirection::RightToLeft, FontFallbackSource::ScriptMatch, 1U, 0U},
        ShapingRunBoundary{5U, kInvalidFontFaceId, ScriptId::Arab, ShapingDirection::RightToLeft, FontFallbackSource::Missing, 1U, 0U},
        ShapingRunBoundary{6U, kInvalidFontFaceId, ScriptId::Latn, ShapingDirection::LeftToRight, FontFallbackSource::Missing, 0U, 0U},
        ShapingRunBoundary{7U, 10U, ScriptId::Latn, ShapingDirection::LeftToRight, FontFallbackSource::Primary, 0U, 0U},
        ShapingRunBoundary{8U, kInvalidFontFaceId, ScriptId::Zzzz, ShapingDirection::LeftToRight, FontFallbackSource::Missing, 0U, 0U}};

    return require(
               std::equal(plan.boundaries.begin(), plan.boundaries.end(), expected.begin(), expected.end()),
               "intersection output is exact") &&
           require(stats.output_runs == 6U, "six shaping runs reported") &&
           require(stats.left_to_right_runs == 4U, "four LTR runs reported") &&
           require(stats.right_to_left_runs == 2U, "two RTL runs reported") &&
           require(stats.missing_font_runs == 2U, "missing-font runs remain explicit") &&
           require(stats.script_splits == 2U, "script splits reported") &&
           require(stats.direction_splits == 2U, "direction splits reported") &&
           require(stats.level_splits == 2U, "level splits reported");
}

bool test_x9_only_cluster_inherits_direction() {
    using namespace zevryon::text;

    const auto codepoints = make_codepoints(3U);
    const auto graphemes = make_singleton_graphemes(3U);
    const std::array scripts{
        ScriptRunBoundary{0U, 0U, ScriptId::Arab, 0U},
        ScriptRunBoundary{3U, 3U, ScriptId::Zzzz, 0U}};
    const std::array fallback{
        FontFallbackBoundary{0U, 4U, FontFallbackSource::Primary, 0U, 0U, 0U},
        FontFallbackBoundary{3U, kInvalidFontFaceId, FontFallbackSource::Missing, 0U, 0U, 0U}};
    auto units = make_units(3U);
    units[1].flags = kBidiUnitRemovedByX9;
    BidiSequenceTopology topology;
    topology.active_unit_indices.push_back(0U);
    topology.active_unit_indices.push_back(2U);
    const std::array<std::uint8_t, 2U> levels{1U, 1U};

    ShapingRunPlan plan;
    ShapingRunPlanStats stats;
    ShapingRunPlanError error;
    return require(
               build_shaping_run_plan(
                   codepoints,
                   graphemes,
                   scripts,
                   fallback,
                   units,
                   topology,
                   levels,
                   1U,
                   &plan,
                   &stats,
                   &error),
               "X9-only cluster fixture builds") &&
           require(plan.boundaries.size() == 2U, "inherited direction does not split run") &&
           require(
               plan.boundaries.front().direction == ShapingDirection::RightToLeft,
               "X9-only cluster inherits RTL direction") &&
           require(stats.inherited_direction_clusters == 1U, "inherited cluster counted");
}

bool test_mixed_cluster_direction_fails_atomically() {
    using namespace zevryon::text;

    const auto codepoints = make_codepoints(2U);
    const std::array graphemes{
        GraphemeBoundary{0U, 0U},
        GraphemeBoundary{2U, 2U}};
    const std::array scripts{
        ScriptRunBoundary{0U, 0U, ScriptId::Latn, 0U},
        ScriptRunBoundary{2U, 1U, ScriptId::Zzzz, 0U}};
    const std::array fallback{
        FontFallbackBoundary{0U, 1U, FontFallbackSource::Primary, 0U, 0U, 0U},
        FontFallbackBoundary{1U, kInvalidFontFaceId, FontFallbackSource::Missing, 0U, 0U, 0U}};
    const auto units = make_units(2U);
    BidiSequenceTopology topology;
    topology.active_unit_indices.push_back(0U);
    topology.active_unit_indices.push_back(1U);
    const std::array<std::uint8_t, 2U> levels{0U, 1U};

    ShapingRunPlan plan;
    plan.boundaries.push_back(ShapingRunBoundary{});
    ShapingRunPlanStats stats;
    ShapingRunPlanError error;
    return require(
               !build_shaping_run_plan(
                   codepoints,
                   graphemes,
                   scripts,
                   fallback,
                   units,
                   topology,
                   levels,
                   0U,
                   &plan,
                   &stats,
                   &error),
               "mixed-direction grapheme is rejected") &&
           require(
               error.kind == ShapingRunPlanErrorKind::MixedClusterDirection,
               "mixed-direction failure is classified") &&
           require(plan.boundaries.empty(), "failed replacement publishes no stale output");
}

bool test_budget_failure_is_atomic() {
    using namespace zevryon::core;
    using namespace zevryon::text;

    const auto codepoints = make_codepoints(2U);
    const auto graphemes = make_singleton_graphemes(2U);
    const std::array scripts{
        ScriptRunBoundary{0U, 0U, ScriptId::Latn, 0U},
        ScriptRunBoundary{2U, 2U, ScriptId::Zzzz, 0U}};
    const std::array fallback{
        FontFallbackBoundary{0U, 1U, FontFallbackSource::Primary, 0U, 0U, 0U},
        FontFallbackBoundary{2U, kInvalidFontFaceId, FontFallbackSource::Missing, 0U, 0U, 0U}};
    const auto units = make_units(2U);
    BidiSequenceTopology topology;
    topology.active_unit_indices.push_back(0U);
    topology.active_unit_indices.push_back(1U);
    const std::array<std::uint8_t, 2U> levels{0U, 0U};

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ShapingRunPlan, 1U);
    LedgerMemoryResource memory(ledger, ResourceClass::ShapingRunPlan);
    ShapingRunPlan plan(&memory);
    ShapingRunPlanStats stats;
    ShapingRunPlanError error;
    return require(
               !build_shaping_run_plan(
                   codepoints,
                   graphemes,
                   scripts,
                   fallback,
                   units,
                   topology,
                   levels,
                   0U,
                   &plan,
                   &stats,
                   &error),
               "one-byte output budget rejects plan") &&
           require(
               error.kind == ShapingRunPlanErrorKind::OutputBudgetExceeded,
               "budget failure is classified") &&
           require(plan.boundaries.empty(), "budget failure publishes no output") &&
           require(
               ledger.snapshot(ResourceClass::ShapingRunPlan).rejected_reservations == 1U,
               "ledger records rejected exact allocation") &&
           require(ledger.accounting_clean(), "budget failure keeps accounting clean");
}

} // namespace

int main() {
    if (!test_intersection_and_sentinel() ||
        !test_x9_only_cluster_inherits_direction() ||
        !test_mixed_cluster_direction_fails_atomically() ||
        !test_budget_failure_is_atomic()) {
        return 1;
    }
    std::cout << "Shaping-run plan tests passed\n";
    return 0;
}
