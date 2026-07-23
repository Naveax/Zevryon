#include "bidi_visual.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <string>
#include <vector>

namespace {

using zevryon::text::BidiClass;
using zevryon::text::BidiExplicitUnit;
using zevryon::text::BidiLineSpan;
using zevryon::text::BidiSequenceTopology;
using zevryon::text::BidiVisualError;
using zevryon::text::BidiVisualErrorKind;
using zevryon::text::BidiVisualOrder;
using zevryon::text::BidiVisualStats;

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

struct CaseData {
    std::vector<BidiExplicitUnit> units;
    std::vector<std::uint8_t> implicit_levels;
    BidiSequenceTopology topology;

    CaseData() : topology(std::pmr::get_default_resource()) {}
};

CaseData make_maximum_case() {
    CaseData data;
    data.implicit_levels = {126U, 126U, 125U, 125U};
    for (std::size_t index = 0U; index < data.implicit_levels.size(); ++index) {
        data.units.push_back(BidiExplicitUnit{
            static_cast<std::uint64_t>(index),
            static_cast<std::uint32_t>(index),
            BidiClass::R,
            BidiClass::R,
            125U,
            0U});
        data.topology.active_unit_indices.push_back(
            static_cast<std::uint32_t>(index));
    }
    return data;
}

bool test_level_126_nested_reversal() {
    CaseData data = make_maximum_case();
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiVisualOrder,
        64U);
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiVisualOrder);
    BidiVisualOrder output(&memory);
    BidiVisualStats stats;
    BidiVisualError error;

    if (!require(
            zevryon::text::resolve_bidi_visual_order(
                data.units,
                data.topology,
                data.implicit_levels,
                1U,
                std::vector<BidiLineSpan>{{0U, 4U}},
                &output,
                &stats,
                &error),
            "level-126 visual order resolves") ||
        !require(
            output.line_levels ==
                std::pmr::vector<std::uint8_t>{126U, 126U, 125U, 125U},
            "level-126 input levels remain intact after L1") ||
        !require(
            output.visual_to_active ==
                std::pmr::vector<std::uint32_t>{3U, 2U, 0U, 1U},
            "L2 descends safely through thresholds 126 and 125") ||
        !require(stats.maximum_input_level == 126U,
                 "maximum input level records 126") ||
        !require(stats.maximum_line_level == 126U,
                 "maximum line level records 126") ||
        !require(stats.l2_reversal_spans == 2U,
                 "two nested reversal spans are reported") ||
        !require(stats.output_levels == 4U &&
                     stats.output_visual_indices == 4U,
                 "one level and visual index are published per active scalar")) {
        return false;
    }

    std::vector<bool> seen(4U, false);
    for (const std::uint32_t active : output.visual_to_active) {
        if (!require(active < seen.size(), "visual index remains in range") ||
            !require(!seen[active], "visual order remains a permutation")) {
            return false;
        }
        seen[active] = true;
    }
    for (const bool value : seen) {
        if (!require(value, "visual permutation covers every active scalar")) {
            return false;
        }
    }

    const auto snapshot = ledger.snapshot(
        zevryon::core::ResourceClass::BidiVisualOrder);
    return require(snapshot.current_bytes == 20U,
                   "four active scalars use exactly twenty output bytes") &&
           require(snapshot.peak_bytes == 20U,
                   "maximum-level stress adds no hidden temporary allocation") &&
           require(snapshot.rejected_reservations == 0U,
                   "maximum-level stress stays within its hard budget") &&
           require(snapshot.accounting_errors == 0U,
                   "maximum-level stress keeps accounting clean");
}

bool test_level_126_budget_failure_is_atomic() {
    CaseData data = make_maximum_case();
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiVisualOrder,
        19U);
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiVisualOrder);
    BidiVisualOrder output(&memory);
    output.line_levels.push_back(7U);
    output.visual_to_active.push_back(7U);
    BidiVisualStats stats;
    BidiVisualError error;

    return require(
               !zevryon::text::resolve_bidi_visual_order(
                   data.units,
                   data.topology,
                   data.implicit_levels,
                   1U,
                   std::vector<BidiLineSpan>{{0U, 4U}},
                   &output,
                   &stats,
                   &error),
               "nineteen-byte visual budget is rejected") &&
           require(error.kind == BidiVisualErrorKind::OutputBudgetExceeded,
                   "maximum-level budget failure has the correct kind") &&
           require(output.line_levels.empty() &&
                       output.visual_to_active.empty(),
                   "budget failure publishes no partial result") &&
           require(
               ledger.snapshot(
                   zevryon::core::ResourceClass::BidiVisualOrder)
                       .rejected_reservations > 0U,
               "maximum-level budget rejection is accounted");
}

} // namespace

int main() {
    if (!test_level_126_nested_reversal() ||
        !test_level_126_budget_failure_is_atomic()) {
        return 1;
    }
    std::cout << "Bidi visual-order level-126 tests passed\n";
    return 0;
}
