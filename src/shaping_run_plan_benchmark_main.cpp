#include "ledger_memory_resource.hpp"
#include "shaping_run_plan.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Fixture {
    std::vector<zevryon::text::DecodedCodePoint> codepoints;
    std::vector<zevryon::text::GraphemeBoundary> graphemes;
    std::vector<zevryon::text::ScriptRunBoundary> scripts;
    std::vector<zevryon::text::FontFallbackBoundary> fallback;
    std::vector<zevryon::text::BidiExplicitUnit> bidi_units;
    zevryon::text::BidiSequenceTopology topology;
    std::vector<std::uint8_t> final_levels;
};

std::size_t parse_size(const char* value, const char* name) {
    try {
        const unsigned long long parsed = std::stoull(value);
        if (parsed == 0ULL ||
            parsed > static_cast<unsigned long long>(
                         std::numeric_limits<std::size_t>::max())) {
            throw std::out_of_range(name);
        }
        return static_cast<std::size_t>(parsed);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
}

Fixture make_fixture(std::size_t scalar_count) {
    using namespace zevryon::text;
    if (scalar_count > static_cast<std::size_t>(
                           std::numeric_limits<std::uint32_t>::max())) {
        throw std::invalid_argument("fixture exceeds uint32_t planner domain");
    }

    Fixture fixture;
    fixture.codepoints.reserve(scalar_count);
    fixture.graphemes.reserve(scalar_count + 1U);
    fixture.bidi_units.reserve(scalar_count);
    fixture.topology.active_unit_indices.reserve(scalar_count);
    fixture.final_levels.reserve(scalar_count);

    for (std::size_t index = 0U; index < scalar_count; ++index) {
        const std::uint32_t compact = static_cast<std::uint32_t>(index);
        const std::uint64_t source = static_cast<std::uint64_t>(index);
        fixture.codepoints.emplace_back(
            static_cast<std::uint32_t>('a' + (index % 26U)),
            source,
            source + 1U,
            false);
        fixture.graphemes.push_back(GraphemeBoundary{source, compact});
        fixture.bidi_units.push_back(BidiExplicitUnit{
            source,
            compact,
            BidiClass::L,
            BidiClass::L,
            0U,
            0U});
        fixture.topology.active_unit_indices.push_back(compact);
        fixture.final_levels.push_back(
            static_cast<std::uint8_t>((index / 32U) % 4U));
    }
    fixture.graphemes.push_back(GraphemeBoundary{
        static_cast<std::uint64_t>(scalar_count),
        static_cast<std::uint32_t>(scalar_count)});

    constexpr ScriptId kScripts[]{ScriptId::Latn, ScriptId::Arab, ScriptId::Hani};
    for (std::size_t cluster = 0U; cluster < scalar_count; cluster += 64U) {
        const std::size_t run_index = cluster / 64U;
        fixture.scripts.push_back(ScriptRunBoundary{
            static_cast<std::uint64_t>(cluster),
            static_cast<std::uint32_t>(cluster),
            kScripts[run_index % 3U],
            0U});
    }
    fixture.scripts.push_back(ScriptRunBoundary{
        static_cast<std::uint64_t>(scalar_count),
        static_cast<std::uint32_t>(scalar_count),
        ScriptId::Zzzz,
        0U});

    for (std::size_t cluster = 0U; cluster < scalar_count; cluster += 48U) {
        const std::size_t run_index = cluster / 48U;
        const bool missing = (run_index % 7U) == 6U;
        fixture.fallback.push_back(FontFallbackBoundary{
            static_cast<std::uint32_t>(cluster),
            missing ? kInvalidFontFaceId
                    : static_cast<FontFaceId>(100U + (run_index % 5U)),
            missing ? FontFallbackSource::Missing
                    : (run_index % 2U == 0U
                           ? FontFallbackSource::Primary
                           : FontFallbackSource::ScriptMatch),
            0U,
            0U,
            0U});
    }
    fixture.fallback.push_back(FontFallbackBoundary{
        static_cast<std::uint32_t>(scalar_count),
        kInvalidFontFaceId,
        FontFallbackSource::Missing,
        0U,
        0U,
        0U});
    return fixture;
}

double percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double last = static_cast<double>(sorted.size() - 1U);
    const double position = last * fraction;
    const std::size_t lower = static_cast<std::size_t>(position);
    const std::size_t upper = std::min(lower + 1U, sorted.size() - 1U);
    const double weight = position - static_cast<double>(lower);
    return sorted[lower] + (sorted[upper] - sorted[lower]) * weight;
}

} // namespace

int main(int argc, char** argv) {
    using namespace zevryon::core;
    using namespace zevryon::text;

    try {
        const std::size_t iterations =
            argc > 1 ? parse_size(argv[1], "iterations") : 512U;
        const std::size_t hard_limit =
            argc > 2 ? parse_size(argv[2], "hard limit") : 49152U;
        constexpr std::size_t kFixtureBytes = 65536U;
        const Fixture fixture = make_fixture(kFixtureBytes);

        ResourceLedger ledger;
        ledger.set_hard_limit(ResourceClass::ShapingRunPlan, hard_limit);
        LedgerMemoryResource memory(ledger, ResourceClass::ShapingRunPlan);
        ShapingRunPlan plan(&memory);
        ShapingRunPlanStats stats;
        ShapingRunPlanError error;
        std::vector<double> samples;
        samples.reserve(iterations);

        for (std::size_t iteration = 0U; iteration < iterations; ++iteration) {
            const auto begin = Clock::now();
            if (!build_shaping_run_plan(
                    fixture.codepoints,
                    fixture.graphemes,
                    fixture.scripts,
                    fixture.fallback,
                    fixture.bidi_units,
                    fixture.topology,
                    fixture.final_levels,
                    0U,
                    &plan,
                    &stats,
                    &error)) {
                std::cerr << "planner failed: "
                          << shaping_run_plan_error_kind_name(error.kind)
                          << ": " << error.message << '\n';
                return 2;
            }
            const auto end = Clock::now();
            samples.push_back(
                std::chrono::duration<double, std::milli>(end - begin).count());
        }

        std::sort(samples.begin(), samples.end());
        const ResourceSnapshot snapshot =
            ledger.snapshot(ResourceClass::ShapingRunPlan);
        const bool passed =
            !plan.boundaries.empty() &&
            plan.boundaries.size() == stats.output_runs + 1U &&
            plan.boundaries.back().cluster_index == kFixtureBytes &&
            stats.script_splits > 0U && stats.font_splits > 0U &&
            stats.direction_splits > 0U && stats.level_splits > 0U &&
            stats.missing_font_runs > 0U &&
            snapshot.current_bytes <= snapshot.peak_bytes &&
            snapshot.peak_bytes <= hard_limit &&
            snapshot.rejected_reservations == 0U &&
            snapshot.accounting_errors == 0U &&
            ledger.within_hard_limits() && ledger.accounting_clean();

        std::cout << std::fixed << std::setprecision(6)
                  << "{\n"
                  << "  \"schema\": \"zevryon.shaping-run-plan-benchmark.v1\",\n"
                  << "  \"fixture_bytes\": " << kFixtureBytes << ",\n"
                  << "  \"input_codepoints\": " << stats.input_codepoints << ",\n"
                  << "  \"input_clusters\": " << stats.input_clusters << ",\n"
                  << "  \"active_bidi_units\": " << stats.active_bidi_units << ",\n"
                  << "  \"iterations\": " << iterations << ",\n"
                  << "  \"boundary_record_bytes\": " << sizeof(ShapingRunBoundary) << ",\n"
                  << "  \"output_runs\": " << stats.output_runs << ",\n"
                  << "  \"output_boundaries\": " << plan.boundaries.size() << ",\n"
                  << "  \"left_to_right_runs\": " << stats.left_to_right_runs << ",\n"
                  << "  \"right_to_left_runs\": " << stats.right_to_left_runs << ",\n"
                  << "  \"missing_font_runs\": " << stats.missing_font_runs << ",\n"
                  << "  \"script_splits\": " << stats.script_splits << ",\n"
                  << "  \"font_splits\": " << stats.font_splits << ",\n"
                  << "  \"direction_splits\": " << stats.direction_splits << ",\n"
                  << "  \"level_splits\": " << stats.level_splits << ",\n"
                  << "  \"p50_ms\": " << percentile(samples, 0.50) << ",\n"
                  << "  \"p95_ms\": " << percentile(samples, 0.95) << ",\n"
                  << "  \"p99_ms\": " << percentile(samples, 0.99) << ",\n"
                  << "  \"maximum_ms\": " << samples.back() << ",\n"
                  << "  \"hard_limit_bytes\": " << hard_limit << ",\n"
                  << "  \"current_bytes\": " << snapshot.current_bytes << ",\n"
                  << "  \"peak_bytes\": " << snapshot.peak_bytes << ",\n"
                  << "  \"rejected_reservations\": " << snapshot.rejected_reservations << ",\n"
                  << "  \"accounting_errors\": " << snapshot.accounting_errors << ",\n"
                  << "  \"within_hard_limits\": "
                  << (ledger.within_hard_limits() ? "true" : "false") << ",\n"
                  << "  \"accounting_clean\": "
                  << (ledger.accounting_clean() ? "true" : "false") << ",\n"
                  << "  \"passed\": " << (passed ? "true" : "false") << "\n"
                  << "}\n";
        return passed ? 0 : 3;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
