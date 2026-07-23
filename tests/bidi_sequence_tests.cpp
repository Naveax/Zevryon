#include "bidi_explicit.hpp"
#include "bidi_sequence.hpp"
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
using zevryon::text::BidiIsolatingRunSequence;
using zevryon::text::BidiLevelRun;
using zevryon::text::BidiParagraphDirection;
using zevryon::text::BidiSequenceError;
using zevryon::text::BidiSequenceErrorKind;
using zevryon::text::BidiSequenceStats;
using zevryon::text::BidiSequenceTopology;
using zevryon::text::DecodedCodePoint;

struct Snapshot {
    std::vector<std::uint32_t> active_unit_indices;
    std::vector<BidiLevelRun> level_runs;
    std::vector<std::uint32_t> sequence_run_indices;
    std::vector<BidiIsolatingRunSequence> sequences;
    BidiSequenceStats stats;
    BidiSequenceError error;
    zevryon::core::ResourceSnapshot resource;
    std::vector<BidiExplicitUnit> explicit_units;
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

bool build(
    const std::vector<DecodedCodePoint>& codepoints,
    Snapshot* snapshot,
    std::size_t sequence_budget = 1U << 20U) {
    zevryon::core::ResourceLedger explicit_ledger;
    explicit_ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiRun,
        2U << 20U);
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
        std::cerr << "explicit setup failed: " << explicit_error.message << '\n';
        return false;
    }

    zevryon::core::ResourceLedger sequence_ledger;
    sequence_ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiSequence,
        sequence_budget);
    zevryon::core::LedgerMemoryResource sequence_memory(
        sequence_ledger,
        zevryon::core::ResourceClass::BidiSequence);
    BidiSequenceTopology topology(&sequence_memory);
    BidiSequenceStats stats;
    BidiSequenceError error;
    const bool result = zevryon::text::build_bidi_isolating_run_sequences(
        explicit_units,
        explicit_stats.paragraph_level,
        &topology,
        &stats,
        &error);

    snapshot->stats = stats;
    snapshot->error = error;
    snapshot->resource = sequence_ledger.snapshot(
        zevryon::core::ResourceClass::BidiSequence);
    snapshot->explicit_units.assign(explicit_units.begin(), explicit_units.end());
    if (result) {
        snapshot->active_unit_indices.assign(
            topology.active_unit_indices.begin(),
            topology.active_unit_indices.end());
        snapshot->level_runs.assign(
            topology.level_runs.begin(),
            topology.level_runs.end());
        snapshot->sequence_run_indices.assign(
            topology.sequence_run_indices.begin(),
            topology.sequence_run_indices.end());
        snapshot->sequences.assign(
            topology.sequences.begin(),
            topology.sequences.end());
    }
    return result;
}

std::vector<std::uint32_t> sequence_units(
    const Snapshot& snapshot,
    std::size_t sequence_index) {
    std::vector<std::uint32_t> output;
    const BidiIsolatingRunSequence& sequence = snapshot.sequences[sequence_index];
    for (std::size_t link = sequence.first_run_link;
         link < static_cast<std::size_t>(sequence.first_run_link) + sequence.run_count;
         ++link) {
        const BidiLevelRun& run =
            snapshot.level_runs[snapshot.sequence_run_indices[link]];
        for (std::size_t active = run.first_active_index;
             active < static_cast<std::size_t>(run.first_active_index) + run.active_count;
             ++active) {
            output.push_back(snapshot.active_unit_indices[active]);
        }
    }
    return output;
}

bool test_embedding_examples() {
    Snapshot snapshot;
    const auto codepoints = make_codepoints({
        'a', 0x202bU, 0x05d0U, 0x202aU, 'b', 0x202cU,
        0x05d1U, 0x202cU, 'c',
    });
    if (!require(build(codepoints, &snapshot), "embedding topology builds") ||
        !require(
            snapshot.active_unit_indices == std::vector<std::uint32_t>({0U, 2U, 4U, 6U, 8U}),
            "X9 removes embedding controls") ||
        !require(snapshot.level_runs.size() == 5U, "five embedding level runs") ||
        !require(snapshot.sequences.size() == 5U, "each embedding run is a sequence")) {
        return false;
    }

    const std::vector<BidiClass> expected_sos{
        BidiClass::L, BidiClass::R, BidiClass::L, BidiClass::L, BidiClass::R,
    };
    const std::vector<BidiClass> expected_eos{
        BidiClass::R, BidiClass::L, BidiClass::L, BidiClass::R, BidiClass::L,
    };
    for (std::size_t index = 0U; index < snapshot.sequences.size(); ++index) {
        if (!require(snapshot.sequences[index].run_count == 1U, "single-run sequence") ||
            !require(snapshot.sequences[index].sos == expected_sos[index], "embedding sos") ||
            !require(snapshot.sequences[index].eos == expected_eos[index], "embedding eos")) {
            return false;
        }
    }
    return true;
}

bool test_nested_isolates() {
    Snapshot snapshot;
    const auto codepoints = make_codepoints({
        'a', 0x2067U, 0x05d0U, 0x2066U, 'b', 0x2069U,
        0x05d1U, 0x2069U, 'c',
    });
    if (!require(build(codepoints, &snapshot), "nested isolate topology builds") ||
        !require(snapshot.stats.matched_isolates == 2U, "two isolate pairs matched") ||
        !require(snapshot.level_runs.size() == 5U, "nested isolate level runs") ||
        !require(snapshot.sequences.size() == 3U, "nested isolate sequences")) {
        return false;
    }

    const std::vector<std::uint32_t> outer{0U, 1U, 7U, 8U};
    const std::vector<std::uint32_t> middle{2U, 3U, 5U, 6U};
    const std::vector<std::uint32_t> inner{4U};
    return require(sequence_units(snapshot, 0U) == outer, "outer isolate sequence chain") &&
           require(sequence_units(snapshot, 1U) == middle, "middle isolate sequence chain") &&
           require(sequence_units(snapshot, 2U) == inner, "inner isolate sequence") &&
           require(snapshot.sequences[0].sos == BidiClass::L, "outer sos") &&
           require(snapshot.sequences[0].eos == BidiClass::L, "outer eos") &&
           require(snapshot.sequences[1].sos == BidiClass::R, "middle sos") &&
           require(snapshot.sequences[1].eos == BidiClass::R, "middle eos") &&
           require(snapshot.sequences[2].sos == BidiClass::L, "inner sos") &&
           require(snapshot.sequences[2].eos == BidiClass::L, "inner eos") &&
           require(snapshot.stats.maximum_sequence_runs == 2U, "two-run maximum") &&
           require(snapshot.stats.maximum_sequence_units == 4U, "four-unit maximum");
}

bool test_unmatched_and_empty_isolates() {
    Snapshot unmatched;
    const auto unmatched_codepoints = make_codepoints({'a', 0x2067U, 0x05d0U});
    if (!require(build(unmatched_codepoints, &unmatched), "unmatched isolate builds") ||
        !require(
            unmatched.stats.unmatched_isolate_initiators == 1U,
            "unmatched initiator counted") ||
        !require(unmatched.sequences.size() == 2U, "unmatched isolate creates two sequences") ||
        !require(unmatched.sequences[0].eos == BidiClass::L, "unmatched isolate eos uses paragraph")) {
        return false;
    }

    Snapshot empty;
    const auto empty_codepoints = make_codepoints({0x2067U, 0x2069U});
    return require(build(empty_codepoints, &empty), "empty isolate builds") &&
           require(empty.stats.matched_isolates == 1U, "empty isolate matched") &&
           require(empty.level_runs.size() == 1U, "empty isolate remains one level run") &&
           require(empty.sequences.size() == 1U, "empty isolate remains one sequence") &&
           require(sequence_units(empty, 0U) == std::vector<std::uint32_t>({0U, 1U}), "empty isolate order");
}

bool test_invalid_input() {
    const auto codepoints = make_codepoints({'a', 0x2067U, 0x05d0U, 0x2069U});

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
            BidiParagraphDirection::Left,
            &units,
            &explicit_stats,
            &explicit_error)) {
        return false;
    }
    units[1].flags = 0U;

    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(zevryon::core::ResourceClass::BidiSequence, 1U << 20U);
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiSequence);
    BidiSequenceTopology topology(&memory);
    BidiSequenceStats stats;
    BidiSequenceError error;
    return require(
               !zevryon::text::build_bidi_isolating_run_sequences(
                   units,
                   explicit_stats.paragraph_level,
                   &topology,
                   &stats,
                   &error),
               "invalid isolate flag rejected") &&
           require(error.kind == BidiSequenceErrorKind::InvalidInput, "invalid input error kind") &&
           require(error.unit_index == 1U, "invalid input index");
}

bool test_hard_budget() {
    Snapshot snapshot;
    const auto codepoints = make_codepoints({
        'a', 0x2067U, 0x05d0U, 0x2066U, 'b', 0x2069U,
        0x05d1U, 0x2069U, 'c',
    });
    return require(!build(codepoints, &snapshot, 1U), "hard cap rejects topology") &&
           require(
               snapshot.error.kind == BidiSequenceErrorKind::OutputBudgetExceeded,
               "hard cap error kind") &&
           require(snapshot.resource.rejected_reservations > 0U, "hard cap rejection counted") &&
           require(snapshot.resource.current_bytes == 0U, "failed topology releases all bytes");
}

} // namespace

int main() {
    if (!test_embedding_examples() ||
        !test_nested_isolates() ||
        !test_unmatched_and_empty_isolates() ||
        !test_invalid_input() ||
        !test_hard_budget()) {
        return 1;
    }
    std::cout << "bidi sequence tests passed\n";
    return 0;
}
