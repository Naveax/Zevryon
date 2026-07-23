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

CaseData make_case(
    const std::vector<BidiClass>& original_types,
    const std::vector<std::uint8_t>& implicit_levels,
    const std::vector<std::uint8_t>& explicit_levels = {}) {
    CaseData data;
    data.implicit_levels = implicit_levels;
    for (std::size_t index = 0U; index < original_types.size(); ++index) {
        const std::uint8_t explicit_level = explicit_levels.empty()
            ? std::uint8_t{0U}
            : explicit_levels[index];
        data.units.push_back(BidiExplicitUnit{
            static_cast<std::uint64_t>(index),
            static_cast<std::uint32_t>(index),
            original_types[index],
            original_types[index],
            explicit_level,
            0U});
        data.topology.active_unit_indices.push_back(
            static_cast<std::uint32_t>(index));
    }
    return data;
}

bool resolve_case(
    CaseData& data,
    std::uint8_t paragraph_level,
    const std::vector<BidiLineSpan>& lines,
    std::size_t budget,
    BidiVisualOrder* output,
    BidiVisualStats* stats,
    BidiVisualError* error,
    zevryon::core::ResourceLedger* ledger) {
    ledger->set_hard_limit(
        zevryon::core::ResourceClass::BidiVisualOrder,
        budget);
    return zevryon::text::resolve_bidi_visual_order(
        data.units,
        data.topology,
        data.implicit_levels,
        paragraph_level,
        lines,
        output,
        stats,
        error);
}

bool test_l1_trailing_and_separator_resets() {
    CaseData trailing = make_case(
        {BidiClass::L, BidiClass::WS, BidiClass::LRI},
        {0U, 2U, 2U});
    zevryon::core::ResourceLedger trailing_ledger;
    zevryon::core::LedgerMemoryResource trailing_memory(
        trailing_ledger,
        zevryon::core::ResourceClass::BidiVisualOrder);
    BidiVisualOrder trailing_output(&trailing_memory);
    BidiVisualStats trailing_stats;
    BidiVisualError trailing_error;
    if (!require(
            resolve_case(
                trailing,
                0U,
                {{0U, 3U}},
                4096U,
                &trailing_output,
                &trailing_stats,
                &trailing_error,
                &trailing_ledger),
            "trailing L1 case resolves") ||
        !require(
            trailing_output.line_levels ==
                std::pmr::vector<std::uint8_t>{0U, 0U, 0U},
            "trailing WS and isolate reset to paragraph level") ||
        !require(
            trailing_stats.l1_whitespace_resets == 1U &&
                trailing_stats.l1_isolate_resets == 1U,
            "trailing L1 counters")) {
        return false;
    }

    CaseData separator = make_case(
        {BidiClass::L, BidiClass::WS, BidiClass::PDI, BidiClass::S},
        {0U, 2U, 2U, 2U});
    zevryon::core::ResourceLedger separator_ledger;
    zevryon::core::LedgerMemoryResource separator_memory(
        separator_ledger,
        zevryon::core::ResourceClass::BidiVisualOrder);
    BidiVisualOrder separator_output(&separator_memory);
    BidiVisualStats separator_stats;
    BidiVisualError separator_error;
    return require(
               resolve_case(
                   separator,
                   0U,
                   {{0U, 4U}},
                   4096U,
                   &separator_output,
                   &separator_stats,
                   &separator_error,
                   &separator_ledger),
               "separator L1 case resolves") &&
           require(
               separator_output.line_levels ==
                   std::pmr::vector<std::uint8_t>{0U, 0U, 0U, 0U},
               "separator and preceding trailing classes reset") &&
           require(
               separator_stats.l1_separator_resets == 1U &&
                   separator_stats.l1_whitespace_resets == 1U &&
                   separator_stats.l1_isolate_resets == 1U,
               "separator L1 counters");
}

bool test_l2_simple_nested_and_lines() {
    CaseData simple = make_case(
        {BidiClass::L, BidiClass::R, BidiClass::R, BidiClass::L},
        {0U, 1U, 1U, 0U});
    zevryon::core::ResourceLedger simple_ledger;
    zevryon::core::LedgerMemoryResource simple_memory(
        simple_ledger,
        zevryon::core::ResourceClass::BidiVisualOrder);
    BidiVisualOrder simple_output(&simple_memory);
    BidiVisualStats simple_stats;
    BidiVisualError simple_error;
    if (!require(
            resolve_case(
                simple,
                0U,
                {{0U, 4U}},
                4096U,
                &simple_output,
                &simple_stats,
                &simple_error,
                &simple_ledger),
            "simple L2 case resolves") ||
        !require(
            simple_output.visual_to_active ==
                std::pmr::vector<std::uint32_t>{0U, 2U, 1U, 3U},
            "simple odd run reverses")) {
        return false;
    }

    CaseData nested = make_case(
        {
            BidiClass::L,
            BidiClass::L,
            BidiClass::L,
            BidiClass::R,
            BidiClass::R,
            BidiClass::L,
        },
        {0U, 2U, 2U, 1U, 1U, 0U});
    zevryon::core::ResourceLedger nested_ledger;
    zevryon::core::LedgerMemoryResource nested_memory(
        nested_ledger,
        zevryon::core::ResourceClass::BidiVisualOrder);
    BidiVisualOrder nested_output(&nested_memory);
    BidiVisualStats nested_stats;
    BidiVisualError nested_error;
    if (!require(
            resolve_case(
                nested,
                0U,
                {{0U, 6U}},
                4096U,
                &nested_output,
                &nested_stats,
                &nested_error,
                &nested_ledger),
            "nested L2 case resolves") ||
        !require(
            nested_output.visual_to_active ==
                std::pmr::vector<std::uint32_t>{0U, 4U, 3U, 1U, 2U, 5U},
            "nested level reversals follow descending thresholds")) {
        return false;
    }

    CaseData multiple = make_case(
        {
            BidiClass::L,
            BidiClass::R,
            BidiClass::R,
            BidiClass::L,
            BidiClass::R,
            BidiClass::R,
        },
        {0U, 1U, 1U, 0U, 1U, 1U});
    zevryon::core::ResourceLedger multiple_ledger;
    zevryon::core::LedgerMemoryResource multiple_memory(
        multiple_ledger,
        zevryon::core::ResourceClass::BidiVisualOrder);
    BidiVisualOrder multiple_output(&multiple_memory);
    BidiVisualStats multiple_stats;
    BidiVisualError multiple_error;
    return require(
               resolve_case(
                   multiple,
                   0U,
                   {{0U, 4U}, {4U, 2U}},
                   4096U,
                   &multiple_output,
                   &multiple_stats,
                   &multiple_error,
                   &multiple_ledger),
               "multiple line case resolves") &&
           require(
               multiple_output.visual_to_active ==
                   std::pmr::vector<std::uint32_t>{0U, 2U, 1U, 3U, 5U, 4U},
               "L2 never reverses across a line boundary") &&
           require(multiple_stats.lines == 2U, "multiple line counter");
}

bool test_l3_combining_sequence_repair() {
    CaseData data = make_case(
        {BidiClass::R, BidiClass::NSM, BidiClass::NSM, BidiClass::R},
        {1U, 1U, 1U, 1U},
        {1U, 1U, 1U, 1U});
    zevryon::core::ResourceLedger ledger;
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiVisualOrder);
    BidiVisualOrder output(&memory);
    BidiVisualStats stats;
    BidiVisualError error;
    return require(
               resolve_case(
                   data,
                   1U,
                   {{0U, 4U}},
                   4096U,
                   &output,
                   &stats,
                   &error,
                   &ledger),
               "L3 combining case resolves") &&
           require(
               output.visual_to_active ==
                   std::pmr::vector<std::uint32_t>{3U, 0U, 1U, 2U},
               "RTL bases reverse while base-plus-NSM order is repaired") &&
           require(stats.l3_combining_sequences == 1U, "L3 sequence counter") &&
           require(stats.l3_repaired_units == 3U, "L3 repaired unit counter");
}

bool test_empty_and_fail_closed_contracts() {
    CaseData empty;
    zevryon::core::ResourceLedger empty_ledger;
    zevryon::core::LedgerMemoryResource empty_memory(
        empty_ledger,
        zevryon::core::ResourceClass::BidiVisualOrder);
    BidiVisualOrder empty_output(&empty_memory);
    BidiVisualStats empty_stats;
    BidiVisualError empty_error;
    if (!require(
            resolve_case(
                empty,
                0U,
                {},
                1U,
                &empty_output,
                &empty_stats,
                &empty_error,
                &empty_ledger),
            "empty visual paragraph resolves") ||
        !require(
            empty_output.line_levels.empty() &&
                empty_output.visual_to_active.empty(),
            "empty visual paragraph has no output")) {
        return false;
    }

    CaseData partition = make_case(
        {BidiClass::L, BidiClass::R},
        {0U, 1U});
    zevryon::core::ResourceLedger partition_ledger;
    zevryon::core::LedgerMemoryResource partition_memory(
        partition_ledger,
        zevryon::core::ResourceClass::BidiVisualOrder);
    BidiVisualOrder partition_output(&partition_memory);
    partition_output.line_levels.push_back(9U);
    partition_output.visual_to_active.push_back(9U);
    BidiVisualStats partition_stats;
    BidiVisualError partition_error;
    if (!require(
            !resolve_case(
                partition,
                0U,
                {{1U, 1U}},
                4096U,
                &partition_output,
                &partition_stats,
                &partition_error,
                &partition_ledger),
            "gapped line partition rejected") ||
        !require(
            partition_error.kind == BidiVisualErrorKind::LinePartitionViolation,
            "line partition failure kind") ||
        !require(
            partition_output.line_levels.empty() &&
                partition_output.visual_to_active.empty(),
            "partition failure publishes no output")) {
        return false;
    }

    CaseData invalid_level = make_case(
        {BidiClass::L},
        {1U},
        {2U});
    zevryon::core::ResourceLedger level_ledger;
    zevryon::core::LedgerMemoryResource level_memory(
        level_ledger,
        zevryon::core::ResourceClass::BidiVisualOrder);
    BidiVisualOrder level_output(&level_memory);
    BidiVisualStats level_stats;
    BidiVisualError level_error;
    if (!require(
            !resolve_case(
                invalid_level,
                0U,
                {{0U, 1U}},
                4096U,
                &level_output,
                &level_stats,
                &level_error,
                &level_ledger),
            "implicit level below explicit level rejected") ||
        !require(
            level_error.kind == BidiVisualErrorKind::InvalidInput,
            "invalid level failure kind")) {
        return false;
    }

    CaseData budget = make_case(
        {BidiClass::L, BidiClass::R, BidiClass::R, BidiClass::L},
        {0U, 1U, 1U, 0U});
    zevryon::core::ResourceLedger budget_ledger;
    zevryon::core::LedgerMemoryResource budget_memory(
        budget_ledger,
        zevryon::core::ResourceClass::BidiVisualOrder);
    BidiVisualOrder budget_output(&budget_memory);
    BidiVisualStats budget_stats;
    BidiVisualError budget_error;
    return require(
               !resolve_case(
                   budget,
                   0U,
                   {{0U, 4U}},
                   1U,
                   &budget_output,
                   &budget_stats,
                   &budget_error,
                   &budget_ledger),
               "tiny visual budget rejected") &&
           require(
               budget_error.kind == BidiVisualErrorKind::OutputBudgetExceeded,
               "visual budget failure kind") &&
           require(
               budget_output.line_levels.empty() &&
                   budget_output.visual_to_active.empty(),
               "budget failure publishes no output") &&
           require(
               budget_ledger.snapshot(
                   zevryon::core::ResourceClass::BidiVisualOrder)
                       .rejected_reservations > 0U,
               "visual budget rejection accounted");
}

} // namespace

int main() {
    if (!test_l1_trailing_and_separator_resets() ||
        !test_l2_simple_nested_and_lines() ||
        !test_l3_combining_sequence_repair() ||
        !test_empty_and_fail_closed_contracts()) {
        return 1;
    }
    std::cout << "Bidi visual-order tests passed\n";
    return 0;
}
