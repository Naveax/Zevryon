#include "bidi_neutral.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"
#include "unicode_bidi_brackets.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <string>
#include <vector>

namespace {

using zevryon::text::BidiClass;
using zevryon::text::BidiExplicitUnit;
using zevryon::text::BidiIsolatingRunSequence;
using zevryon::text::BidiLevelRun;
using zevryon::text::BidiNeutralError;
using zevryon::text::BidiNeutralErrorKind;
using zevryon::text::BidiNeutralStats;
using zevryon::text::BidiSequenceTopology;
using zevryon::text::DecodedCodePoint;

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

struct CaseData {
    std::vector<DecodedCodePoint> codepoints;
    std::vector<BidiExplicitUnit> units;
    std::vector<BidiClass> weak_types;
    BidiSequenceTopology topology;

    CaseData() : topology(std::pmr::get_default_resource()) {}
};

CaseData make_case(
    const std::vector<std::uint32_t>& values,
    const std::vector<BidiClass>& original_types,
    const std::vector<BidiClass>& weak_types,
    std::uint8_t level,
    BidiClass sos,
    BidiClass eos) {
    CaseData data;
    data.weak_types = weak_types;
    std::uint64_t source = 0U;
    for (std::size_t index = 0U; index < values.size(); ++index) {
        data.codepoints.emplace_back(values[index], source, source + 1U, false);
        data.units.push_back(BidiExplicitUnit{
            source,
            static_cast<std::uint32_t>(index),
            original_types[index],
            original_types[index],
            level,
            0U});
        data.topology.active_unit_indices.push_back(
            static_cast<std::uint32_t>(index));
        ++source;
    }
    data.topology.level_runs.push_back(BidiLevelRun{
        0U,
        static_cast<std::uint32_t>(values.size()),
        level,
        0U,
        0U});
    data.topology.sequence_run_indices.push_back(0U);
    data.topology.sequences.push_back(BidiIsolatingRunSequence{
        0U,
        1U,
        sos,
        eos,
        level,
        0U,
        0U});
    return data;
}

bool resolve_case(
    CaseData& data,
    std::size_t budget,
    std::pmr::vector<BidiClass>* output,
    BidiNeutralStats* stats,
    BidiNeutralError* error,
    zevryon::core::ResourceLedger* ledger) {
    ledger->set_hard_limit(
        zevryon::core::ResourceClass::BidiNeutralResolution,
        budget);
    return zevryon::text::resolve_bidi_neutral_types(
        data.codepoints,
        data.units,
        data.topology,
        data.weak_types,
        output,
        stats,
        error);
}

bool test_bracket_data() {
    const auto opening = zevryon::text::bidi_bracket_info('(');
    const auto closing = zevryon::text::bidi_bracket_info(')');
    return require(
               opening.type == zevryon::text::BidiBracketType::Open &&
                   opening.paired_codepoint == ')',
               "opening bracket mapping") &&
           require(
               closing.type == zevryon::text::BidiBracketType::Close &&
                   closing.paired_codepoint == '(',
               "closing bracket mapping") &&
           require(
               zevryon::text::bidi_brackets_match(0x3009U, 0x232AU) &&
                   zevryon::text::bidi_brackets_match(0x232AU, 0x3009U),
               "canonical angle-bracket equivalence");
}

bool test_n0_embedding_direction() {
    CaseData data = make_case(
        {'(', 'a', ')'},
        {BidiClass::ON, BidiClass::L, BidiClass::ON},
        {BidiClass::ON, BidiClass::L, BidiClass::ON},
        0U,
        BidiClass::L,
        BidiClass::L);
    zevryon::core::ResourceLedger ledger;
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiNeutralResolution);
    std::pmr::vector<BidiClass> output(&memory);
    BidiNeutralStats stats;
    BidiNeutralError error;
    return require(
               resolve_case(data, 4096U, &output, &stats, &error, &ledger),
               "N0 embedding case resolves") &&
           require(
               output == std::pmr::vector<BidiClass>{
                   BidiClass::L, BidiClass::L, BidiClass::L},
               "N0 bracket pair takes embedding direction") &&
           require(stats.n0_embedding_pairs == 1U, "N0 embedding counter");
}

bool test_n0_opposite_context() {
    CaseData data = make_case(
        {'x', '(', 0x05D0U, ')'},
        {BidiClass::R, BidiClass::ON, BidiClass::R, BidiClass::ON},
        {BidiClass::R, BidiClass::ON, BidiClass::R, BidiClass::ON},
        0U,
        BidiClass::L,
        BidiClass::L);
    zevryon::core::ResourceLedger ledger;
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiNeutralResolution);
    std::pmr::vector<BidiClass> output(&memory);
    BidiNeutralStats stats;
    BidiNeutralError error;
    return require(
               resolve_case(data, 4096U, &output, &stats, &error, &ledger),
               "N0 opposite case resolves") &&
           require(
               output[1] == BidiClass::R && output[3] == BidiClass::R,
               "preceding opposite strong makes pair opposite") &&
           require(stats.n0_opposite_pairs == 1U, "N0 opposite counter");
}

bool test_n0_nsm_following() {
    CaseData data = make_case(
        {'(', 0x0301U, 'a', ')', 0x0301U},
        {BidiClass::ON, BidiClass::NSM, BidiClass::L, BidiClass::ON, BidiClass::NSM},
        {BidiClass::ON, BidiClass::R, BidiClass::L, BidiClass::ON, BidiClass::R},
        0U,
        BidiClass::L,
        BidiClass::L);
    zevryon::core::ResourceLedger ledger;
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiNeutralResolution);
    std::pmr::vector<BidiClass> output(&memory);
    BidiNeutralStats stats;
    BidiNeutralError error;
    return require(
               resolve_case(data, 4096U, &output, &stats, &error, &ledger),
               "N0 NSM case resolves") &&
           require(
               output[0] == BidiClass::L && output[1] == BidiClass::L &&
                   output[3] == BidiClass::L && output[4] == BidiClass::L,
               "NSMs immediately following changed brackets inherit direction") &&
           require(stats.n0_following_nsm_changes == 2U, "N0 NSM counter");
}

bool test_n1_and_n2() {
    CaseData n1 = make_case(
        {'a', ' ', 'b'},
        {BidiClass::L, BidiClass::WS, BidiClass::L},
        {BidiClass::L, BidiClass::WS, BidiClass::L},
        0U,
        BidiClass::L,
        BidiClass::L);
    zevryon::core::ResourceLedger ledger1;
    zevryon::core::LedgerMemoryResource memory1(
        ledger1,
        zevryon::core::ResourceClass::BidiNeutralResolution);
    std::pmr::vector<BidiClass> output1(&memory1);
    BidiNeutralStats stats1;
    BidiNeutralError error1;
    if (!require(
            resolve_case(n1, 4096U, &output1, &stats1, &error1, &ledger1),
            "N1 case resolves") ||
        !require(output1[1] == BidiClass::L, "N1 equal boundaries") ||
        !require(stats1.n1_changes == 1U, "N1 counter")) {
        return false;
    }

    CaseData n2 = make_case(
        {0x05D0U, ' ', 'a'},
        {BidiClass::R, BidiClass::WS, BidiClass::L},
        {BidiClass::R, BidiClass::WS, BidiClass::L},
        1U,
        BidiClass::R,
        BidiClass::R);
    zevryon::core::ResourceLedger ledger2;
    zevryon::core::LedgerMemoryResource memory2(
        ledger2,
        zevryon::core::ResourceClass::BidiNeutralResolution);
    std::pmr::vector<BidiClass> output2(&memory2);
    BidiNeutralStats stats2;
    BidiNeutralError error2;
    return require(
               resolve_case(n2, 4096U, &output2, &stats2, &error2, &ledger2),
               "N2 case resolves") &&
           require(output2[1] == BidiClass::R, "N2 uses embedding level parity") &&
           require(stats2.n2_changes == 1U, "N2 counter");
}

bool test_bracket_overflow() {
    std::vector<std::uint32_t> values(64U, static_cast<std::uint32_t>('('));
    values.push_back(static_cast<std::uint32_t>('a'));
    values.insert(values.end(), 64U, static_cast<std::uint32_t>(')'));
    std::vector<BidiClass> original(values.size(), BidiClass::ON);
    std::vector<BidiClass> weak(values.size(), BidiClass::ON);
    original[64U] = BidiClass::L;
    weak[64U] = BidiClass::L;
    CaseData data = make_case(
        values,
        original,
        weak,
        0U,
        BidiClass::L,
        BidiClass::L);
    zevryon::core::ResourceLedger ledger;
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiNeutralResolution);
    std::pmr::vector<BidiClass> output(&memory);
    BidiNeutralStats stats;
    BidiNeutralError error;
    return require(
               resolve_case(data, 16384U, &output, &stats, &error, &ledger),
               "overflow sequence still completes N1/N2") &&
           require(stats.bracket_overflow_sequences == 1U, "BD16 overflow visible") &&
           require(stats.bracket_pairs == 0U, "BD16 overflow publishes no pairs");
}

bool test_fail_closed_budget_and_topology() {
    CaseData data = make_case(
        {'a', ' ', 'b'},
        {BidiClass::L, BidiClass::WS, BidiClass::L},
        {BidiClass::L, BidiClass::WS, BidiClass::L},
        0U,
        BidiClass::L,
        BidiClass::L);
    zevryon::core::ResourceLedger ledger;
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiNeutralResolution);
    std::pmr::vector<BidiClass> output(&memory);
    output.push_back(BidiClass::R);
    BidiNeutralStats stats;
    BidiNeutralError error;
    if (!require(
            !resolve_case(data, 1U, &output, &stats, &error, &ledger),
            "tiny budget rejected") ||
        !require(output.empty(), "budget failure publishes no partial output") ||
        !require(
            error.kind == BidiNeutralErrorKind::OutputBudgetExceeded,
            "budget failure kind")) {
        return false;
    }

    CaseData broken = make_case(
        {'a'},
        {BidiClass::L},
        {BidiClass::L},
        0U,
        BidiClass::L,
        BidiClass::L);
    broken.topology.level_runs[0].active_count = 2U;
    zevryon::core::ResourceLedger broken_ledger;
    zevryon::core::LedgerMemoryResource broken_memory(
        broken_ledger,
        zevryon::core::ResourceClass::BidiNeutralResolution);
    std::pmr::vector<BidiClass> broken_output(&broken_memory);
    BidiNeutralStats broken_stats;
    BidiNeutralError broken_error;
    return require(
               !resolve_case(
                   broken,
                   4096U,
                   &broken_output,
                   &broken_stats,
                   &broken_error,
                   &broken_ledger),
               "broken topology rejected") &&
           require(
               broken_error.kind == BidiNeutralErrorKind::TopologyViolation,
               "topology failure kind") &&
           require(broken_output.empty(), "topology failure publishes no output");
}

} // namespace

int main() {
    if (!test_bracket_data() ||
        !test_n0_embedding_direction() ||
        !test_n0_opposite_context() ||
        !test_n0_nsm_following() ||
        !test_n1_and_n2() ||
        !test_bracket_overflow() ||
        !test_fail_closed_budget_and_topology()) {
        return 1;
    }
    std::cout << "Bidi neutral tests passed\n";
    return 0;
}
