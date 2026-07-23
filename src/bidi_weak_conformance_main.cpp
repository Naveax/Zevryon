#include "bidi_explicit.hpp"
#include "bidi_sequence.hpp"
#include "bidi_weak.hpp"
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
using zevryon::text::BidiExplicitError;
using zevryon::text::BidiExplicitStats;
using zevryon::text::BidiExplicitUnit;
using zevryon::text::BidiParagraphDirection;
using zevryon::text::BidiSequenceError;
using zevryon::text::BidiSequenceStats;
using zevryon::text::BidiSequenceTopology;
using zevryon::text::BidiWeakError;
using zevryon::text::BidiWeakStats;
using zevryon::text::DecodedCodePoint;

struct RuleCase {
    const char* name;
    std::vector<std::uint32_t> codepoints;
    std::vector<BidiClass> expected;
};

std::uint8_t utf8_length(std::uint32_t value) noexcept {
    return value <= 0x7fU ? 1U
         : value <= 0x7ffU ? 2U
         : value <= 0xffffU ? 3U
         : 4U;
}

std::vector<DecodedCodePoint> decode_fixture(
    const std::vector<std::uint32_t>& values) {
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

bool run_case(
    const RuleCase& item,
    BidiWeakStats* aggregate) {
    const auto codepoints = decode_fixture(item.codepoints);

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
        std::cerr << item.name << ": explicit stage failed: "
                  << explicit_error.message << '\n';
        return false;
    }

    zevryon::core::ResourceLedger sequence_ledger;
    sequence_ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiSequence,
        1U * 1024U * 1024U);
    zevryon::core::LedgerMemoryResource sequence_memory(
        sequence_ledger,
        zevryon::core::ResourceClass::BidiSequence);
    BidiSequenceTopology topology(&sequence_memory);
    BidiSequenceStats sequence_stats;
    BidiSequenceError sequence_error;
    if (!zevryon::text::build_bidi_isolating_run_sequences(
            units,
            explicit_stats.paragraph_level,
            &topology,
            &sequence_stats,
            &sequence_error)) {
        std::cerr << item.name << ": sequence stage failed: "
                  << sequence_error.message << '\n';
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
    if (!zevryon::text::resolve_bidi_weak_types(
            units,
            topology,
            &types,
            &stats,
            &error)) {
        std::cerr << item.name << ": weak stage failed: " << error.message << '\n';
        return false;
    }
    if (types.size() != item.expected.size() ||
        !std::equal(types.begin(), types.end(), item.expected.begin())) {
        std::cerr << item.name << ": weak result mismatch\n";
        return false;
    }

    aggregate->w1_nsm_changes += stats.w1_nsm_changes;
    aggregate->w2_en_to_an += stats.w2_en_to_an;
    aggregate->w3_al_to_r += stats.w3_al_to_r;
    aggregate->w4_separator_changes += stats.w4_separator_changes;
    aggregate->w5_et_to_en += stats.w5_et_to_en;
    aggregate->w6_neutralized += stats.w6_neutralized;
    aggregate->w7_en_to_l += stats.w7_en_to_l;
    return true;
}

} // namespace

int main() {
    const std::vector<RuleCase> cases{
        {"W1-start", {0x0301U, 'a'}, {BidiClass::L, BidiClass::L}},
        {"W1-after-PDI", {0x2067U, 0x05d0U, 0x2069U, 0x0301U},
         {BidiClass::RLI, BidiClass::R, BidiClass::PDI, BidiClass::ON}},
        {"W2", {0x0627U, '1'}, {BidiClass::R, BidiClass::AN}},
        {"W3", {0x0627U}, {BidiClass::R}},
        {"W4-ES", {0x05d0U, '1', '+', '2'},
         {BidiClass::R, BidiClass::EN, BidiClass::EN, BidiClass::EN}},
        {"W4-CS-EN", {0x05d0U, '1', ',', '2'},
         {BidiClass::R, BidiClass::EN, BidiClass::EN, BidiClass::EN}},
        {"W4-CS-AN", {0x0627U, 0x0661U, ',', 0x0662U},
         {BidiClass::R, BidiClass::AN, BidiClass::AN, BidiClass::AN}},
        {"W5-before", {0x05d0U, '$', '$', '1'},
         {BidiClass::R, BidiClass::EN, BidiClass::EN, BidiClass::EN}},
        {"W5-after", {0x05d0U, '1', '$', '$'},
         {BidiClass::R, BidiClass::EN, BidiClass::EN, BidiClass::EN}},
        {"W6", {0x05d0U, '+', '$', ','},
         {BidiClass::R, BidiClass::ON, BidiClass::ON, BidiClass::ON}},
        {"W7-left", {'a', '1'}, {BidiClass::L, BidiClass::L}},
        {"W7-right", {0x05d0U, '1'}, {BidiClass::R, BidiClass::EN}},
        {"isolating-sequences", {'a', 0x2067U, 0x0627U, '1', 0x2069U, '2'},
         {BidiClass::L, BidiClass::RLI, BidiClass::R, BidiClass::AN,
          BidiClass::PDI, BidiClass::L}},
    };

    BidiWeakStats aggregate;
    for (const RuleCase& item : cases) {
        if (!run_case(item, &aggregate)) {
            return 1;
        }
    }

    std::cout << '{'
              << "\"schema\":\"zevryon.bidi-weak-conformance.v1\","
              << "\"uax_version\":\"Unicode 17.0.0 / UAX #9 revision 51\","
              << "\"rules\":7,"
              << "\"cases\":" << cases.size() << ','
              << "\"w1_changes\":" << aggregate.w1_nsm_changes << ','
              << "\"w2_changes\":" << aggregate.w2_en_to_an << ','
              << "\"w3_changes\":" << aggregate.w3_al_to_r << ','
              << "\"w4_changes\":" << aggregate.w4_separator_changes << ','
              << "\"w5_changes\":" << aggregate.w5_et_to_en << ','
              << "\"w6_changes\":" << aggregate.w6_neutralized << ','
              << "\"w7_changes\":" << aggregate.w7_en_to_l << ','
              << "\"passed\":true}\n";
    return 0;
}
