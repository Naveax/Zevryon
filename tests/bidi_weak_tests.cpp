#include "bidi_explicit.hpp"
#include "bidi_sequence.hpp"
#include "bidi_weak.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory_resource>
#include <string>
#include <vector>

namespace {

using zevryon::text::BidiClass;
using zevryon::text::BidiExplicitError;
using zevryon::text::BidiExplicitStats;
using zevryon::text::BidiExplicitUnit;
using zevryon::text::BidiParagraphDirection;
using zevryon::text::BidiSequenceError;
using zevryon::text::BidiSequenceStats;
using zevryon::text::BidiSequenceTopology;
using zevryon::text::BidiWeakError;
using zevryon::text::BidiWeakErrorKind;
using zevryon::text::BidiWeakStats;
using zevryon::text::DecodedCodePoint;

struct Result {
    std::vector<BidiClass> types;
    std::vector<std::uint32_t> active_unit_indices;
    BidiWeakStats stats;
    BidiWeakError error;
    zevryon::core::ResourceSnapshot resource;
};

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

std::uint8_t utf8_length(std::uint32_t value) noexcept {
    return value <= 0x7fU ? 1U
         : value <= 0x7ffU ? 2U
         : value <= 0xffffU ? 3U
         : 4U;
}

std::vector<DecodedCodePoint> make_codepoints(
    std::initializer_list<std::uint32_t> values) {
    std::vector<DecodedCodePoint> output;
    output.reserve(values.size());
    std::uint64_t source = 0U;
    for (const std::uint32_t value : values) {
        const std::uint8_t length = utf8_length(value);
        output.emplace_back(value, source, source + length, false);
        source += length;
    }
    return output;
}

bool resolve(
    const std::vector<DecodedCodePoint>& codepoints,
    Result* result,
    std::size_t weak_budget = 64U * 1024U,
    BidiSequenceTopology** topology_out = nullptr,
    std::pmr::memory_resource** topology_resource_out = nullptr) {
    zevryon::core::ResourceLedger explicit_ledger;
    explicit_ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiRun,
        2U * 1024U * 1024U);
    zevryon::core::LedgerMemoryResource explicit_memory(
        explicit_ledger,
        zevryon::core::ResourceClass::BidiRun);
    std::pmr::vector<BidiExplicitUnit> units(&explicit_memory);
    BidiExplicitStats explicit_stats;
    BidiExplicitError explicit_error;
    if (!zevryon::text::resolve_bidi_explicit(
            codepoints,
            BidiParagraphDirection::Auto,
            &units,
            &explicit_stats,
            &explicit_error)) {
        std::cerr << "explicit setup failed: " << explicit_error.message << '\n';
        return false;
    }

    zevryon::core::ResourceLedger topology_ledger;
    topology_ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiSequence,
        1U * 1024U * 1024U);
    auto* topology_memory = new zevryon::core::LedgerMemoryResource(
        topology_ledger,
        zevryon::core::ResourceClass::BidiSequence);
    auto* topology = new BidiSequenceTopology(topology_memory);
    BidiSequenceStats topology_stats;
    BidiSequenceError topology_error;
    if (!zevryon::text::build_bidi_isolating_run_sequences(
            units,
            explicit_stats.paragraph_level,
            topology,
            &topology_stats,
            &topology_error)) {
        std::cerr << "topology setup failed: " << topology_error.message << '\n';
        delete topology;
        delete topology_memory;
        return false;
    }

    zevryon::core::ResourceLedger weak_ledger;
    weak_ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiTypeResolution,
        weak_budget);
    zevryon::core::LedgerMemoryResource weak_memory(
        weak_ledger,
        zevryon::core::ResourceClass::BidiTypeResolution);
    std::pmr::vector<BidiClass> types(&weak_memory);
    BidiWeakStats stats;
    BidiWeakError error;
    const bool success = zevryon::text::resolve_bidi_weak_types(
        units,
        *topology,
        &types,
        &stats,
        &error);

    result->stats = stats;
    result->error = error;
    result->resource = weak_ledger.snapshot(
        zevryon::core::ResourceClass::BidiTypeResolution);
    result->active_unit_indices.assign(
        topology->active_unit_indices.begin(),
        topology->active_unit_indices.end());
    if (success) {
        result->types.assign(types.begin(), types.end());
    }

    if (topology_out != nullptr && topology_resource_out != nullptr) {
        *topology_out = topology;
        *topology_resource_out = topology_memory;
    } else {
        delete topology;
        delete topology_memory;
    }
    return success;
}

bool expect_types(
    std::initializer_list<std::uint32_t> codepoints,
    std::initializer_list<BidiClass> expected,
    Result* result,
    const std::string& label) {
    if (!require(resolve(make_codepoints(codepoints), result), label + " resolves")) {
        return false;
    }
    return require(
        result->types == std::vector<BidiClass>(expected),
        label + " resolved types");
}

bool test_w1() {
    Result start;
    if (!expect_types(
            {0x0301U, 'a'},
            {BidiClass::L, BidiClass::L},
            &start,
            "W1 sequence-start NSM") ||
        !require(start.stats.w1_nsm_changes == 1U, "W1 start count")) {
        return false;
    }

    Result after_pdi;
    return expect_types(
               {0x2067U, 0x05d0U, 0x2069U, 0x0301U},
               {BidiClass::RLI, BidiClass::R, BidiClass::PDI, BidiClass::ON},
               &after_pdi,
               "W1 NSM after PDI") &&
           require(after_pdi.stats.w1_nsm_changes == 1U, "W1 PDI count");
}

bool test_w2_w3() {
    Result result;
    return expect_types(
               {0x0627U, '1'},
               {BidiClass::R, BidiClass::AN},
               &result,
               "W2 and W3 Arabic number") &&
           require(result.stats.w2_en_to_an == 1U, "W2 count") &&
           require(result.stats.w3_al_to_r == 1U, "W3 count");
}

bool test_w4() {
    Result european;
    if (!expect_types(
            {0x05d0U, '1', '+', '2'},
            {BidiClass::R, BidiClass::EN, BidiClass::EN, BidiClass::EN},
            &european,
            "W4 European separator") ||
        !require(european.stats.w4_separator_changes == 1U, "W4 ES count")) {
        return false;
    }

    Result arabic;
    if (!expect_types(
            {0x0627U, 0x0661U, ',', 0x0662U},
            {BidiClass::R, BidiClass::AN, BidiClass::AN, BidiClass::AN},
            &arabic,
            "W4 Arabic common separator") ||
        !require(arabic.stats.w4_separator_changes == 1U, "W4 CS count")) {
        return false;
    }

    Result mismatch;
    return expect_types(
               {0x05d0U, '1', ',', 0x0661U},
               {BidiClass::R, BidiClass::EN, BidiClass::ON, BidiClass::AN},
               &mismatch,
               "W4 mismatched numbers") &&
           require(mismatch.stats.w6_neutralized == 1U, "W6 neutralizes mismatched CS");
}

bool test_w5_w6() {
    Result adjacent_before;
    if (!expect_types(
            {0x05d0U, '1', '$', '$', '('},
            {BidiClass::R, BidiClass::EN, BidiClass::EN, BidiClass::EN, BidiClass::ON},
            &adjacent_before,
            "W5 ET after EN") ||
        !require(adjacent_before.stats.w5_et_to_en == 2U, "W5 after count")) {
        return false;
    }

    Result adjacent_after;
    if (!expect_types(
            {0x05d0U, '$', '$', '1'},
            {BidiClass::R, BidiClass::EN, BidiClass::EN, BidiClass::EN},
            &adjacent_after,
            "W5 ET before EN") ||
        !require(adjacent_after.stats.w5_et_to_en == 2U, "W5 before count")) {
        return false;
    }

    Result remaining;
    return expect_types(
               {0x05d0U, '+', '$', ','},
               {BidiClass::R, BidiClass::ON, BidiClass::ON, BidiClass::ON},
               &remaining,
               "W6 remaining separators") &&
           require(remaining.stats.w6_neutralized == 3U, "W6 count");
}

bool test_w7() {
    Result result;
    return expect_types(
               {'a', '1', 0x05d0U, '2'},
               {BidiClass::L, BidiClass::L, BidiClass::R, BidiClass::EN},
               &result,
               "W7 European number context") &&
           require(result.stats.w7_en_to_l == 1U, "W7 count");
}

bool test_isolating_sequence_independence() {
    Result result;
    return expect_types(
               {'a', 0x2067U, 0x0627U, '1', 0x2069U, '2'},
               {BidiClass::L, BidiClass::RLI, BidiClass::R, BidiClass::AN,
                BidiClass::PDI, BidiClass::L},
               &result,
               "weak rules stay inside isolating sequences") &&
           require(result.stats.isolating_sequences == 2U, "two isolating sequences") &&
           require(result.stats.w2_en_to_an == 1U, "inner W2 count") &&
           require(result.stats.w7_en_to_l == 1U, "outer W7 count");
}

bool test_hard_budget() {
    Result result;
    return require(
               !resolve(make_codepoints({'a', '1', 0x05d0U}), &result, 1U),
               "weak hard cap rejects output") &&
           require(
               result.error.kind == BidiWeakErrorKind::OutputBudgetExceeded,
               "weak hard cap error kind") &&
           require(result.resource.rejected_reservations > 0U, "weak hard cap rejection counted") &&
           require(result.resource.current_bytes == 0U, "weak hard cap leaves no bytes");
}

bool test_topology_validation() {
    const auto codepoints = make_codepoints({'a', 0x2067U, 0x05d0U, 0x2069U});
    Result baseline;
    BidiSequenceTopology* topology = nullptr;
    std::pmr::memory_resource* topology_resource = nullptr;
    if (!require(
            resolve(codepoints, &baseline, 64U * 1024U, &topology, &topology_resource),
            "topology validation baseline")) {
        return false;
    }

    topology->sequence_run_indices[0] = static_cast<std::uint32_t>(
        topology->level_runs.size());

    zevryon::core::ResourceLedger explicit_ledger;
    explicit_ledger.set_hard_limit(zevryon::core::ResourceClass::BidiRun, 1U << 20U);
    zevryon::core::LedgerMemoryResource explicit_memory(
        explicit_ledger,
        zevryon::core::ResourceClass::BidiRun);
    std::pmr::vector<BidiExplicitUnit> units(&explicit_memory);
    BidiExplicitStats explicit_stats;
    BidiExplicitError explicit_error;
    if (!zevryon::text::resolve_bidi_explicit(
            codepoints,
            BidiParagraphDirection::Auto,
            &units,
            &explicit_stats,
            &explicit_error)) {
        delete topology;
        delete topology_resource;
        return false;
    }

    zevryon::core::ResourceLedger weak_ledger;
    weak_ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiTypeResolution,
        64U * 1024U);
    zevryon::core::LedgerMemoryResource weak_memory(
        weak_ledger,
        zevryon::core::ResourceClass::BidiTypeResolution);
    std::pmr::vector<BidiClass> types(&weak_memory);
    BidiWeakStats stats;
    BidiWeakError error;
    const bool rejected = !zevryon::text::resolve_bidi_weak_types(
        units,
        *topology,
        &types,
        &stats,
        &error);
    delete topology;
    delete topology_resource;
    return require(rejected, "invalid topology rejected") &&
           require(error.kind == BidiWeakErrorKind::TopologyViolation, "topology error kind") &&
           require(types.empty(), "invalid topology publishes no output");
}

} // namespace

int main() {
    if (!test_w1() ||
        !test_w2_w3() ||
        !test_w4() ||
        !test_w5_w6() ||
        !test_w7() ||
        !test_isolating_sequence_independence() ||
        !test_hard_budget() ||
        !test_topology_validation()) {
        return 1;
    }
    std::cout << "bidi weak-type tests passed\n";
    return 0;
}
