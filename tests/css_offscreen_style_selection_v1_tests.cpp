#include "css_offscreen_style_selection_v1.hpp"
#include "css_style_materialization_v1.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <span>
#include <string_view>

namespace {

using zevryon::core::LedgerMemoryResource;
using zevryon::core::ResourceClass;
using zevryon::core::ResourceLedger;
using namespace zevryon::style;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

bool select(
    std::span<const CssOffscreenStyleCandidateV1> candidates,
    CssOffscreenStyleSelectionV1* output,
    CssOffscreenStyleSelectionStatsV1* stats,
    CssOffscreenStyleSelectionErrorV1* error,
    CssOffscreenStyleSelectionConfigV1 config = {}) {
    return select_css_offscreen_style_nodes_v1(
        candidates,
        config,
        output,
        stats,
        error);
}

bool test_deterministic_nearest_selection() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssOffscreenStyleSelectionV1 output(&memory);
    CssOffscreenStyleSelectionStatsV1 stats;
    CssOffscreenStyleSelectionErrorV1 error;

    const std::array<CssOffscreenStyleCandidateV1, 7> candidates{{
        {10U, 50U, false},
        {20U, 20U, false},
        {10U, 10U, false},
        {30U, 20U, false},
        {40U, 101U, false},
        {50U, 0U, true},
        {5U, 20U, false},
    }};
    CssOffscreenStyleSelectionConfigV1 config;
    config.lookahead_css_px = 100U;
    config.maximum_selected_styles = 3U;

    bool ok = expect(
        select(
            candidates,
            &output,
            &stats,
            &error,
            config),
        "bounded offscreen selection must succeed");
    ok &= expect(error.kind == CssOffscreenStyleSelectionErrorKindV1::None,
                 "successful selection must clear error");
    ok &= expect(output.terminal_nodes.size() == 3U,
                 "selection cap must retain three styles");
    if (output.terminal_nodes.size() == 3U) {
        ok &= expect(
            output.terminal_nodes[0] == 10U &&
                output.terminal_nodes[1] == 5U &&
                output.terminal_nodes[2] == 20U,
            "selection must order by nearest distance then terminal ID");
    }
    ok &= expect(stats.candidates_considered == 7U,
                 "candidate count stats must be exact");
    ok &= expect(stats.visible_candidates_skipped == 1U,
                 "visible candidate must be skipped");
    ok &= expect(stats.outside_lookahead_skipped == 1U,
                 "outside-lookahead candidate must be skipped");
    ok &= expect(stats.duplicate_terminal_candidates == 1U,
                 "duplicate terminal must collapse to nearest occurrence");
    ok &= expect(stats.eligible_unique_styles == 4U,
                 "four unique offscreen styles must remain eligible");
    ok &= expect(stats.selected_styles == 3U,
                 "selected style stats must match cap");
    return ok;
}

bool test_selection_output_is_materializer_input() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);

    CssOffscreenStyleSelectionV1 selection(&memory);
    CssOffscreenStyleSelectionStatsV1 selection_stats;
    CssOffscreenStyleSelectionErrorV1 selection_error;
    const std::array<CssOffscreenStyleCandidateV1, 1> candidates{{
        {0U, 12U, false},
    }};

    bool ok = expect(
        select(
            candidates,
            &selection,
            &selection_stats,
            &selection_error),
        "root style must be selectable for offscreen materialization");

    CssComputedStyleDagV1 dag(&memory);
    dag.nodes.push_back(CssStyleDagNodeV1{});

    CssStyleMaterializationBatchV1 batch(&memory);
    CssStyleMaterializationStatsV1 materialization_stats;
    CssStyleMaterializationErrorV1 materialization_error;
    ok &= expect(
        materialize_css_style_nodes_v1(
            dag,
            selection.terminal_nodes,
            CssStyleMaterializationConfigV1{},
            &batch,
            &materialization_stats,
            &materialization_error),
        "selection output must feed materializer directly");
    ok &= expect(batch.styles.size() == 1U,
                 "selected root style must materialize once");
    if (!batch.styles.empty()) {
        ok &= expect(batch.styles.front().terminal_node == 0U &&
                         batch.styles.front().property_count == 0U,
                     "selected root style must remain empty after materialization");
    }
    return ok;
}

bool test_failure_preserves_previous_output() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssOffscreenStyleSelectionV1 output(&memory);
    CssOffscreenStyleSelectionStatsV1 stats;
    CssOffscreenStyleSelectionErrorV1 error;

    const std::array<CssOffscreenStyleCandidateV1, 1> baseline{{
        {7U, 1U, false},
    }};
    bool ok = expect(
        select(baseline, &output, &stats, &error),
        "selection baseline must succeed");
    ok &= expect(output.terminal_nodes.size() == 1U &&
                     output.terminal_nodes.front() == 7U,
                 "selection baseline output must be exact");

    const std::array<CssOffscreenStyleCandidateV1, 2> two{{
        {1U, 1U, false},
        {2U, 1U, false},
    }};
    CssOffscreenStyleSelectionConfigV1 config;
    config.maximum_candidates = 1U;
    config.maximum_selected_styles = 1U;
    ok &= expect(
        !select(
            two,
            &output,
            &stats,
            &error,
            config),
        "candidate count above limit must fail");
    ok &= expect(
        error.kind ==
            CssOffscreenStyleSelectionErrorKindV1::CandidateLimitExceeded,
        "candidate limit must report exact error");
    ok &= expect(output.terminal_nodes.size() == 1U &&
                     output.terminal_nodes.front() == 7U,
                 "candidate-limit failure must preserve prior output");

    config = CssOffscreenStyleSelectionConfigV1{};
    config.maximum_work_units = 1U;
    ok &= expect(
        !select(
            two,
            &output,
            &stats,
            &error,
            config),
        "work budget must fail closed");
    ok &= expect(
        error.kind ==
            CssOffscreenStyleSelectionErrorKindV1::WorkBudgetExceeded,
        "work budget must report exact error");
    ok &= expect(output.terminal_nodes.size() == 1U &&
                     output.terminal_nodes.front() == 7U,
                 "work-budget failure must preserve prior output");
    return ok;
}

bool test_zero_lookahead_and_empty_batch() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssOffscreenStyleSelectionV1 output(&memory);
    CssOffscreenStyleSelectionStatsV1 stats;
    CssOffscreenStyleSelectionErrorV1 error;

    CssOffscreenStyleSelectionConfigV1 config;
    config.lookahead_css_px = 0U;
    const std::array<CssOffscreenStyleCandidateV1, 3> candidates{{
        {1U, 0U, false},
        {2U, 1U, false},
        {3U, 0U, true},
    }};
    bool ok = expect(
        select(
            candidates,
            &output,
            &stats,
            &error,
            config),
        "zero lookahead selection must remain valid");
    ok &= expect(output.terminal_nodes.size() == 1U &&
                     output.terminal_nodes.front() == 1U,
                 "zero lookahead must keep only zero-distance offscreen style");

    const std::array<CssOffscreenStyleCandidateV1, 0> empty{};
    ok &= expect(
        select(empty, &output, &stats, &error, config),
        "empty candidate batch must succeed");
    ok &= expect(output.terminal_nodes.empty(),
                 "empty candidate batch must publish empty output");
    return ok;
}

bool test_real_ledger_rejection() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssOffscreenStyleSelectionV1 output(&memory);
    CssOffscreenStyleSelectionStatsV1 stats;
    CssOffscreenStyleSelectionErrorV1 error;
    const std::array<CssOffscreenStyleCandidateV1, 2> candidates{{
        {1U, 1U, false},
        {2U, 2U, false},
    }};

    bool ok = expect(
        !select(
            candidates,
            &output,
            &stats,
            &error),
        "real selection ledger rejection must fail");
    ok &= expect(
        error.kind ==
            CssOffscreenStyleSelectionErrorKindV1::AllocationFailure,
        "real selection ledger rejection must report allocation-failure");
    ok &= expect(
        ledger.snapshot(ResourceClass::ComputedStyle)
                .rejected_reservations > 0U,
        "selection ledger rejection must be visible");
    ok &= expect(
        ledger.accounting_clean(),
        "selection ledger rejection accounting must remain clean");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_deterministic_nearest_selection();
    ok &= test_selection_output_is_materializer_input();
    ok &= test_failure_preserves_previous_output();
    ok &= test_zero_lookahead_and_empty_batch();
    ok &= test_real_ledger_rejection();
    if (!ok) {
        return 1;
    }
    std::cout << "CSS offscreen style selection v1 tests passed\n";
    return 0;
}
