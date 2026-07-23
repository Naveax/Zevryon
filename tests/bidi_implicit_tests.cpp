#include "bidi_implicit.hpp"
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
using zevryon::text::BidiImplicitError;
using zevryon::text::BidiImplicitErrorKind;
using zevryon::text::BidiImplicitStats;
using zevryon::text::BidiIsolatingRunSequence;
using zevryon::text::BidiLevelRun;
using zevryon::text::BidiSequenceTopology;

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

struct CaseData {
    std::vector<BidiExplicitUnit> units;
    std::vector<BidiClass> types;
    BidiSequenceTopology topology;

    CaseData() : topology(std::pmr::get_default_resource()) {}
};

CaseData make_case(
    const std::vector<std::uint8_t>& levels,
    const std::vector<BidiClass>& types) {
    CaseData data;
    data.types = types;
    for (std::size_t index = 0U; index < levels.size(); ++index) {
        data.units.push_back(BidiExplicitUnit{
            static_cast<std::uint64_t>(index),
            static_cast<std::uint32_t>(index),
            types[index],
            types[index],
            levels[index],
            0U});
        data.topology.active_unit_indices.push_back(
            static_cast<std::uint32_t>(index));
    }

    std::size_t first = 0U;
    while (first < levels.size()) {
        std::size_t end = first + 1U;
        while (end < levels.size() && levels[end] == levels[first]) {
            ++end;
        }
        const std::uint32_t run_id = static_cast<std::uint32_t>(
            data.topology.level_runs.size());
        data.topology.level_runs.push_back(BidiLevelRun{
            static_cast<std::uint32_t>(first),
            static_cast<std::uint32_t>(end - first),
            levels[first],
            0U,
            0U});
        data.topology.sequence_run_indices.push_back(run_id);
        data.topology.sequences.push_back(BidiIsolatingRunSequence{
            run_id,
            1U,
            (levels[first] & 1U) == 0U ? BidiClass::L : BidiClass::R,
            (levels[first] & 1U) == 0U ? BidiClass::L : BidiClass::R,
            levels[first],
            0U,
            0U});
        first = end;
    }
    return data;
}

bool resolve_case(
    CaseData& data,
    std::size_t budget,
    std::pmr::vector<std::uint8_t>* output,
    BidiImplicitStats* stats,
    BidiImplicitError* error,
    zevryon::core::ResourceLedger* ledger) {
    ledger->set_hard_limit(
        zevryon::core::ResourceClass::BidiImplicitLevel,
        budget);
    return zevryon::text::resolve_bidi_implicit_levels(
        data.units,
        data.topology,
        data.types,
        output,
        stats,
        error);
}

bool test_i1_even_levels() {
    CaseData data = make_case(
        {0U, 0U, 0U, 0U},
        {BidiClass::L, BidiClass::R, BidiClass::EN, BidiClass::AN});
    zevryon::core::ResourceLedger ledger;
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiImplicitLevel);
    std::pmr::vector<std::uint8_t> output(&memory);
    BidiImplicitStats stats;
    BidiImplicitError error;
    return require(
               resolve_case(data, 4096U, &output, &stats, &error, &ledger),
               "I1 even case resolves") &&
           require(
               output.size() == 4U && output[0] == 0U && output[1] == 1U &&
                   output[2] == 2U && output[3] == 2U,
               "I1 level table") &&
           require(stats.i1_r_changes == 1U, "I1 R counter") &&
           require(stats.i1_number_changes == 2U, "I1 number counter") &&
           require(stats.maximum_output_level == 2U, "I1 maximum output");
}

bool test_i2_odd_levels() {
    CaseData data = make_case(
        {1U, 1U, 1U, 1U},
        {BidiClass::L, BidiClass::R, BidiClass::EN, BidiClass::AN});
    zevryon::core::ResourceLedger ledger;
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiImplicitLevel);
    std::pmr::vector<std::uint8_t> output(&memory);
    BidiImplicitStats stats;
    BidiImplicitError error;
    return require(
               resolve_case(data, 4096U, &output, &stats, &error, &ledger),
               "I2 odd case resolves") &&
           require(
               output.size() == 4U && output[0] == 2U && output[1] == 1U &&
                   output[2] == 2U && output[3] == 2U,
               "I2 level table") &&
           require(stats.i2_l_changes == 1U, "I2 L counter") &&
           require(stats.i2_number_changes == 2U, "I2 number counter") &&
           require(stats.maximum_output_level == 2U, "I2 maximum output");
}

bool test_maximum_levels_and_multiple_runs() {
    CaseData data = make_case(
        {124U, 124U, 125U, 125U, 125U},
        {
            BidiClass::EN,
            BidiClass::R,
            BidiClass::L,
            BidiClass::AN,
            BidiClass::R,
        });
    zevryon::core::ResourceLedger ledger;
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiImplicitLevel);
    std::pmr::vector<std::uint8_t> output(&memory);
    BidiImplicitStats stats;
    BidiImplicitError error;
    return require(
               resolve_case(data, 4096U, &output, &stats, &error, &ledger),
               "maximum-level case resolves") &&
           require(
               output.size() == 5U && output[0] == 126U && output[1] == 125U &&
                   output[2] == 126U && output[3] == 126U && output[4] == 125U,
               "max_depth+1 is bounded at 126") &&
           require(stats.maximum_input_level == 125U, "maximum explicit level") &&
           require(stats.maximum_output_level == 126U, "maximum implicit level") &&
           require(stats.isolating_sequences == 2U, "multiple sequence count");
}

bool test_empty_input() {
    CaseData data;
    zevryon::core::ResourceLedger ledger;
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiImplicitLevel);
    std::pmr::vector<std::uint8_t> output(&memory);
    BidiImplicitStats stats;
    BidiImplicitError error;
    return require(
               resolve_case(data, 1U, &output, &stats, &error, &ledger),
               "empty paragraph resolves") &&
           require(output.empty(), "empty paragraph has no output") &&
           require(stats.output_levels == 0U, "empty output counter");
}

bool test_fail_closed_stage_topology_and_budget() {
    CaseData invalid_stage = make_case({0U}, {BidiClass::ON});
    zevryon::core::ResourceLedger stage_ledger;
    zevryon::core::LedgerMemoryResource stage_memory(
        stage_ledger,
        zevryon::core::ResourceClass::BidiImplicitLevel);
    std::pmr::vector<std::uint8_t> stage_output(&stage_memory);
    stage_output.push_back(7U);
    BidiImplicitStats stage_stats;
    BidiImplicitError stage_error;
    if (!require(
            !resolve_case(
                invalid_stage,
                4096U,
                &stage_output,
                &stage_stats,
                &stage_error,
                &stage_ledger),
            "pre-N2 neutral type rejected") ||
        !require(stage_output.empty(), "stage failure publishes no output") ||
        !require(
            stage_error.kind == BidiImplicitErrorKind::InvalidInput,
            "stage failure kind")) {
        return false;
    }

    CaseData broken = make_case({0U, 1U}, {BidiClass::L, BidiClass::R});
    broken.topology.sequence_run_indices[1] = 0U;
    zevryon::core::ResourceLedger broken_ledger;
    zevryon::core::LedgerMemoryResource broken_memory(
        broken_ledger,
        zevryon::core::ResourceClass::BidiImplicitLevel);
    std::pmr::vector<std::uint8_t> broken_output(&broken_memory);
    BidiImplicitStats broken_stats;
    BidiImplicitError broken_error;
    if (!require(
            !resolve_case(
                broken,
                4096U,
                &broken_output,
                &broken_stats,
                &broken_error,
                &broken_ledger),
            "duplicate run link rejected") ||
        !require(
            broken_error.kind == BidiImplicitErrorKind::TopologyViolation,
            "topology failure kind") ||
        !require(broken_output.empty(), "topology failure publishes no output")) {
        return false;
    }

    CaseData budget = make_case(
        {0U, 0U, 0U, 0U},
        {BidiClass::L, BidiClass::R, BidiClass::EN, BidiClass::AN});
    zevryon::core::ResourceLedger budget_ledger;
    zevryon::core::LedgerMemoryResource budget_memory(
        budget_ledger,
        zevryon::core::ResourceClass::BidiImplicitLevel);
    std::pmr::vector<std::uint8_t> budget_output(&budget_memory);
    BidiImplicitStats budget_stats;
    BidiImplicitError budget_error;
    return require(
               !resolve_case(
                   budget,
                   1U,
                   &budget_output,
                   &budget_stats,
                   &budget_error,
                   &budget_ledger),
               "tiny implicit budget rejected") &&
           require(
               budget_error.kind == BidiImplicitErrorKind::OutputBudgetExceeded,
               "budget failure kind") &&
           require(budget_output.empty(), "budget failure publishes no output") &&
           require(
               budget_ledger.snapshot(
                   zevryon::core::ResourceClass::BidiImplicitLevel)
                       .rejected_reservations > 0U,
               "budget rejection is accounted");
}

} // namespace

int main() {
    if (!test_i1_even_levels() ||
        !test_i2_odd_levels() ||
        !test_maximum_levels_and_multiple_runs() ||
        !test_empty_input() ||
        !test_fail_closed_stage_topology_and_budget()) {
        return 1;
    }
    std::cout << "Bidi implicit-level tests passed\n";
    return 0;
}
