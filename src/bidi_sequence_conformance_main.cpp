#include "bidi_explicit.hpp"
#include "bidi_sequence.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory_resource>
#include <string_view>
#include <vector>

namespace {

using zevryon::text::BidiClass;
using zevryon::text::BidiExplicitError;
using zevryon::text::BidiExplicitStats;
using zevryon::text::BidiExplicitUnit;
using zevryon::text::BidiIsolatingRunSequence;
using zevryon::text::BidiLevelRun;
using zevryon::text::BidiParagraphDirection;
using zevryon::text::BidiSequenceError;
using zevryon::text::BidiSequenceStats;
using zevryon::text::BidiSequenceTopology;
using zevryon::text::DecodedCodePoint;

struct ExpectedSequence {
    std::vector<std::uint32_t> unit_indices;
    std::uint8_t level{0};
    BidiClass sos{BidiClass::L};
    BidiClass eos{BidiClass::L};
};

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

std::vector<std::uint32_t> sequence_units(
    const BidiSequenceTopology& topology,
    std::size_t sequence_index) {
    std::vector<std::uint32_t> output;
    const BidiIsolatingRunSequence& sequence = topology.sequences[sequence_index];
    for (std::size_t link = sequence.first_run_link;
         link < static_cast<std::size_t>(sequence.first_run_link) + sequence.run_count;
         ++link) {
        const BidiLevelRun& run =
            topology.level_runs[topology.sequence_run_indices[link]];
        for (std::size_t active = run.first_active_index;
             active < static_cast<std::size_t>(run.first_active_index) + run.active_count;
             ++active) {
            output.push_back(topology.active_unit_indices[active]);
        }
    }
    return output;
}

void print_units(const std::vector<std::uint32_t>& units) {
    std::cerr << '[';
    for (std::size_t index = 0U; index < units.size(); ++index) {
        if (index != 0U) {
            std::cerr << ',';
        }
        std::cerr << units[index];
    }
    std::cerr << ']';
}

bool check_example(
    std::string_view name,
    const std::vector<DecodedCodePoint>& codepoints,
    const std::vector<ExpectedSequence>& expected,
    std::uint64_t* sequence_total) {
    zevryon::core::ResourceLedger explicit_ledger;
    explicit_ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiRun,
        1U << 20U);
    zevryon::core::LedgerMemoryResource explicit_memory(
        explicit_ledger,
        zevryon::core::ResourceClass::BidiRun);
    std::pmr::vector<BidiExplicitUnit> explicit_units(&explicit_memory);
    BidiExplicitStats explicit_stats;
    BidiExplicitError explicit_error;
    if (!zevryon::text::resolve_bidi_explicit(
            codepoints,
            BidiParagraphDirection::Left,
            &explicit_units,
            &explicit_stats,
            &explicit_error)) {
        std::cerr << name << ": explicit stage failed: "
                  << explicit_error.message << '\n';
        return false;
    }

    zevryon::core::ResourceLedger sequence_ledger;
    sequence_ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiSequence,
        1U << 20U);
    zevryon::core::LedgerMemoryResource sequence_memory(
        sequence_ledger,
        zevryon::core::ResourceClass::BidiSequence);
    BidiSequenceTopology topology(&sequence_memory);
    BidiSequenceStats stats;
    BidiSequenceError error;
    if (!zevryon::text::build_bidi_isolating_run_sequences(
            explicit_units,
            explicit_stats.paragraph_level,
            &topology,
            &stats,
            &error)) {
        std::cerr << name << ": topology failed: " << error.message << '\n';
        return false;
    }
    if (topology.sequences.size() != expected.size()) {
        std::cerr << name << ": sequence count mismatch: expected "
                  << expected.size() << " got " << topology.sequences.size() << '\n';
        return false;
    }

    for (std::size_t index = 0U; index < expected.size(); ++index) {
        const BidiIsolatingRunSequence& actual = topology.sequences[index];
        const std::vector<std::uint32_t> actual_units =
            sequence_units(topology, index);
        if (actual_units != expected[index].unit_indices ||
            actual.level != expected[index].level ||
            actual.sos != expected[index].sos ||
            actual.eos != expected[index].eos) {
            std::cerr << name << ": sequence " << index << " mismatch; expected ";
            print_units(expected[index].unit_indices);
            std::cerr << " level=" << static_cast<unsigned>(expected[index].level)
                      << " sos=" << static_cast<unsigned>(expected[index].sos)
                      << " eos=" << static_cast<unsigned>(expected[index].eos)
                      << " actual ";
            print_units(actual_units);
            std::cerr << " level=" << static_cast<unsigned>(actual.level)
                      << " sos=" << static_cast<unsigned>(actual.sos)
                      << " eos=" << static_cast<unsigned>(actual.eos) << '\n';
            return false;
        }
    }
    *sequence_total += static_cast<std::uint64_t>(topology.sequences.size());
    return true;
}

} // namespace

int main() {
    std::uint64_t sequence_total = 0U;

    // Unicode 17.0.0 UAX #9 BD13 Example 1:
    // text1 RLE text2 PDF RLE text3 PDF text4.
    const bool example1 = check_example(
        "UAX9-BD13-example-1",
        make_codepoints({
            'a', 0x202bU, 0x05d0U, 0x202cU,
            0x202bU, 0x05d1U, 0x202cU, 'b',
        }),
        {
            {{0U}, 0U, BidiClass::L, BidiClass::R},
            {{2U, 5U}, 1U, BidiClass::R, BidiClass::R},
            {{7U}, 0U, BidiClass::R, BidiClass::L},
        },
        &sequence_total);

    // Unicode 17.0.0 UAX #9 BD13 Example 2:
    // text1 RLI text2 PDI RLI text3 PDI text4.
    const bool example2 = check_example(
        "UAX9-BD13-example-2",
        make_codepoints({
            'a', 0x2067U, 0x05d0U, 0x2069U,
            0x2067U, 0x05d1U, 0x2069U, 'b',
        }),
        {
            {{0U, 1U, 3U, 4U, 6U, 7U}, 0U, BidiClass::L, BidiClass::L},
            {{2U}, 1U, BidiClass::R, BidiClass::R},
            {{5U}, 1U, BidiClass::R, BidiClass::R},
        },
        &sequence_total);

    // Unicode 17.0.0 UAX #9 BD13 Example 3:
    // text1 RLI text2 LRI text3 RLE text4 PDF text5 PDI text6 PDI text7.
    const bool example3 = check_example(
        "UAX9-BD13-example-3",
        make_codepoints({
            'a', 0x2067U, 0x05d0U, 0x2066U, 'b',
            0x202bU, 0x05d1U, 0x202cU, 'c', 0x2069U,
            0x05d2U, 0x2069U, 'd',
        }),
        {
            {{0U, 1U, 11U, 12U}, 0U, BidiClass::L, BidiClass::L},
            {{2U, 3U, 9U, 10U}, 1U, BidiClass::R, BidiClass::R},
            {{4U}, 2U, BidiClass::L, BidiClass::R},
            {{6U}, 3U, BidiClass::R, BidiClass::R},
            {{8U}, 2U, BidiClass::R, BidiClass::R},
        },
        &sequence_total);

    if (!example1 || !example2 || !example3) {
        return 1;
    }

    std::cout << '{'
              << "\"schema\":\"zevryon.bidi-sequence-conformance.v1\","
              << "\"uax_version\":\"Unicode 17.0.0 / UAX #9 revision 51\","
              << "\"examples\":3,"
              << "\"sequences\":" << sequence_total << ','
              << "\"passed\":true}\n";
    return 0;
}
