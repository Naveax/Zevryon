#include "grapheme_segmenter.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"
#include "script_run.hpp"
#include "unicode_script.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory_resource>
#include <string>
#include <vector>

namespace {

using zevryon::text::DecodedCodePoint;
using zevryon::text::GraphemeBoundary;
using zevryon::text::GraphemeError;
using zevryon::text::GraphemeSegmentStats;
using zevryon::text::ScriptId;
using zevryon::text::ScriptRunBoundary;
using zevryon::text::ScriptRunError;
using zevryon::text::ScriptRunErrorKind;
using zevryon::text::ScriptRunStats;

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
    std::initializer_list<std::uint32_t> values,
    std::uint64_t source_base = 0U) {
    std::vector<DecodedCodePoint> output;
    output.reserve(values.size());
    std::uint64_t source = source_base;
    for (const std::uint32_t value : values) {
        const std::uint8_t length = utf8_length(value);
        output.emplace_back(value, source, source + length, false);
        source += length;
    }
    return output;
}

bool make_graphemes(
    const std::vector<DecodedCodePoint>& codepoints,
    std::vector<GraphemeBoundary>* output) {
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(
        zevryon::core::ResourceClass::GraphemeCluster,
        1U << 20U);
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::GraphemeCluster);
    std::pmr::vector<GraphemeBoundary> boundaries(&memory);
    GraphemeSegmentStats stats;
    GraphemeError error;
    if (!zevryon::text::segment_graphemes(
            codepoints,
            &boundaries,
            &stats,
            &error)) {
        std::cerr << "grapheme setup failed: " << error.message << '\n';
        return false;
    }
    output->assign(boundaries.begin(), boundaries.end());
    return true;
}

bool resolve(
    const std::vector<DecodedCodePoint>& codepoints,
    const std::vector<GraphemeBoundary>& graphemes,
    std::vector<ScriptRunBoundary>* output,
    ScriptRunStats* stats,
    ScriptRunError* error,
    std::size_t budget = 1U << 20U,
    zevryon::core::ResourceSnapshot* snapshot = nullptr) {
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(zevryon::core::ResourceClass::ScriptRun, budget);
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::ScriptRun);
    std::pmr::vector<ScriptRunBoundary> boundaries(&memory);
    const bool result = zevryon::text::resolve_script_runs(
        codepoints,
        graphemes,
        &boundaries,
        stats,
        error);
    if (snapshot != nullptr) {
        *snapshot = ledger.snapshot(zevryon::core::ResourceClass::ScriptRun);
    }
    if (result) {
        output->assign(boundaries.begin(), boundaries.end());
    }
    return result;
}

bool expect_runs(
    std::initializer_list<std::uint32_t> values,
    std::initializer_list<ScriptId> expected_scripts,
    std::initializer_list<std::uint32_t> expected_cluster_counts,
    const std::string& label) {
    const auto codepoints = make_codepoints(values, 100U);
    std::vector<GraphemeBoundary> graphemes;
    if (!make_graphemes(codepoints, &graphemes)) {
        return false;
    }
    std::vector<ScriptRunBoundary> runs;
    ScriptRunStats stats;
    ScriptRunError error;
    if (!require(
            resolve(codepoints, graphemes, &runs, &stats, &error),
            label + " resolves") ||
        !require(
            runs.size() == expected_scripts.size() + 1U,
            label + " boundary count") ||
        !require(
            stats.output_runs == expected_scripts.size(),
            label + " stats run count")) {
        return false;
    }

    auto script = expected_scripts.begin();
    auto count = expected_cluster_counts.begin();
    std::size_t index = 0U;
    for (; script != expected_scripts.end() &&
           count != expected_cluster_counts.end();
         ++script, ++count, ++index) {
        if (!require(runs[index].script == *script, label + " script") ||
            !require(
                runs[index + 1U].cluster_index - runs[index].cluster_index ==
                    *count,
                label + " cluster count")) {
            return false;
        }
    }
    return require(script == expected_scripts.end(), label + " script arity") &&
           require(count == expected_cluster_counts.end(), label + " count arity") &&
           require(runs.front().source_offset == 100U, label + " source start") &&
           require(
               runs.back().source_offset == codepoints.back().source_end(),
               label + " source sentinel") &&
           require(
               runs.back().cluster_index == graphemes.size() - 1U,
               label + " cluster sentinel") &&
           require(runs.back().script == ScriptId::Zzzz, label + " sentinel script");
}

bool test_property_lookup() {
    const auto prolonged = zevryon::text::script_extensions(0x30fcU);
    ScriptId parsed = ScriptId::Zzzz;
    return require(zevryon::text::script_of('A') == ScriptId::Latn, "Latin property") &&
           require(zevryon::text::script_of(0x03b1U) == ScriptId::Grek, "Greek property") &&
           require(zevryon::text::script_of(0x0627U) == ScriptId::Arab, "Arabic property") &&
           require(zevryon::text::script_of(0x4e00U) == ScriptId::Hani, "Han property") &&
           require(zevryon::text::script_of(0x3042U) == ScriptId::Hira, "Hiragana property") &&
           require(zevryon::text::script_of(0x0301U) == ScriptId::Zinh, "Inherited property") &&
           require(zevryon::text::script_of(0x0378U) == ScriptId::Zzzz, "Unknown property") &&
           require(prolonged.has_explicit_extensions(), "explicit extensions present") &&
           require(prolonged.contains(ScriptId::Hira), "extension contains Hiragana") &&
           require(prolonged.contains(ScriptId::Kana), "extension contains Katakana") &&
           require(
               zevryon::text::script_short_name(ScriptId::Latn) == "Latn",
               "short script name") &&
           require(
               zevryon::text::script_long_name(ScriptId::Latn) == "Latin",
               "long script name") &&
           require(
               zevryon::text::script_id_from_name("Latin", &parsed) &&
                   parsed == ScriptId::Latn,
               "parse long script name");
}

bool test_run_resolution() {
    return expect_runs(
               {'a', 'b', 'c', ',', ' ', '1', '2'},
               {ScriptId::Latn},
               {7U},
               "Latin with neutrals") &&
           expect_runs(
               {'(', 0x03b1U},
               {ScriptId::Grek},
               {2U},
               "leading neutral adopts following script") &&
           expect_runs(
               {'a', 0x03b1U, 0x0431U},
               {ScriptId::Latn, ScriptId::Grek, ScriptId::Cyrl},
               {1U, 1U, 1U},
               "three-script split") &&
           expect_runs(
               {'a', 0x0301U, 0x03b1U},
               {ScriptId::Latn, ScriptId::Grek},
               {1U, 1U},
               "combining mark remains grapheme atomic") &&
           expect_runs(
               {0x3042U, 0x30fcU, 0x3044U},
               {ScriptId::Hira},
               {3U},
               "Script_Extensions intersection") &&
           expect_runs(
               {'1', '2', ' ', '-', '3'},
               {ScriptId::Zyyy},
               {5U},
               "all-neutral fallback");
}

bool test_internal_grapheme_conflict() {
    const auto codepoints = make_codepoints({'a', 0x03b1U});
    const std::vector<GraphemeBoundary> graphemes{
        {codepoints.front().source_start, 0U},
        {codepoints.back().source_end(), 2U},
    };
    std::vector<ScriptRunBoundary> runs;
    ScriptRunStats stats;
    ScriptRunError error;
    return require(
               resolve(codepoints, graphemes, &runs, &stats, &error),
               "mixed-script grapheme resolves") &&
           require(runs.size() == 2U, "mixed-script grapheme one run") &&
           require(runs.front().script == ScriptId::Latn, "first strong scalar wins") &&
           require(stats.internal_cluster_conflicts == 1U, "internal conflict counted");
}

bool test_invalid_input() {
    const auto codepoints = make_codepoints({'a', 'b'});
    std::vector<GraphemeBoundary> graphemes;
    if (!make_graphemes(codepoints, &graphemes)) {
        return false;
    }
    graphemes.back().source_offset += 1U;
    std::vector<ScriptRunBoundary> runs;
    ScriptRunStats stats;
    ScriptRunError error;
    return require(
               !resolve(codepoints, graphemes, &runs, &stats, &error),
               "invalid sentinel rejected") &&
           require(error.kind == ScriptRunErrorKind::InvalidInput, "invalid sentinel error kind");
}

bool test_hard_budget() {
    const auto codepoints = make_codepoints({'a', 0x03b1U, 0x0431U});
    std::vector<GraphemeBoundary> graphemes;
    if (!make_graphemes(codepoints, &graphemes)) {
        return false;
    }
    std::vector<ScriptRunBoundary> runs;
    ScriptRunStats stats;
    ScriptRunError error;
    zevryon::core::ResourceSnapshot snapshot;
    return require(
               !resolve(
                   codepoints,
                   graphemes,
                   &runs,
                   &stats,
                   &error,
                   1U,
                   &snapshot),
               "script-run hard cap rejects output") &&
           require(
               error.kind == ScriptRunErrorKind::OutputBudgetExceeded,
               "hard cap error kind") &&
           require(snapshot.rejected_reservations > 0U, "hard cap rejection counted") &&
           require(snapshot.current_bytes == 0U, "hard cap leaves no resident bytes");
}

} // namespace

int main() {
    if (!test_property_lookup() ||
        !test_run_resolution() ||
        !test_internal_grapheme_conflict() ||
        !test_invalid_input() ||
        !test_hard_budget()) {
        return 1;
    }
    std::cout << "script run tests passed\n";
    return 0;
}
