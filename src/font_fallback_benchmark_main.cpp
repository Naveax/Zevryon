#include "font_catalog.hpp"
#include "font_fallback.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"
#include "unicode_script.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using zevryon::core::LedgerMemoryResource;
using zevryon::core::ResourceClass;
using zevryon::core::ResourceLedger;
using namespace zevryon::text;

constexpr std::size_t kFixtureBytes = 64U * 1024U;
constexpr std::size_t kFaceCount = 256U;
constexpr std::size_t kWarmupIterations = 8U;

struct Fixture {
    std::vector<DecodedCodePoint> codepoints;
    std::vector<GraphemeBoundary> graphemes;
    std::vector<ScriptRunBoundary> script_runs;
    std::uint64_t source_bytes{0};
};

ScriptId require_script(const char* name) {
    ScriptId script = ScriptId::Zzzz;
    if (!script_id_from_name(name, &script)) {
        std::cerr << "missing script: " << name << '\n';
        std::exit(2);
    }
    return script;
}

std::uint64_t utf8_length(std::uint32_t codepoint) noexcept {
    if (codepoint <= 0x7fU) {
        return 1U;
    }
    if (codepoint <= 0x7ffU) {
        return 2U;
    }
    if (codepoint <= 0xffffU) {
        return 3U;
    }
    return 4U;
}

bool append_cluster(
    Fixture* fixture,
    std::span<const std::uint32_t> values,
    ScriptId script) {
    std::uint64_t source_bytes = 0U;
    for (const std::uint32_t value : values) {
        source_bytes += utf8_length(value);
    }
    if (fixture->source_bytes + source_bytes > kFixtureBytes) {
        return false;
    }

    const std::uint32_t cluster_index = static_cast<std::uint32_t>(
        fixture->graphemes.size() - 1U);
    if (fixture->script_runs.empty() ||
        fixture->script_runs.back().script != script) {
        fixture->script_runs.push_back(ScriptRunBoundary{
            fixture->source_bytes,
            cluster_index,
            script,
            0U});
    }

    for (const std::uint32_t value : values) {
        const std::uint64_t length = utf8_length(value);
        fixture->codepoints.emplace_back(
            value,
            fixture->source_bytes,
            fixture->source_bytes + length,
            false);
        fixture->source_bytes += length;
    }
    fixture->graphemes.push_back(GraphemeBoundary{
        fixture->source_bytes,
        static_cast<std::uint32_t>(fixture->codepoints.size())});
    return true;
}

Fixture make_fixture(const std::array<ScriptId, 8>& scripts) {
    Fixture fixture;
    fixture.codepoints.reserve(32768U);
    fixture.graphemes.reserve(24576U);
    fixture.script_runs.reserve(24576U);
    fixture.graphemes.push_back({0U, 0U});

    const std::array<std::uint32_t, 1> latin{{0x0061U}};
    const std::array<std::uint32_t, 2> latin_mark{{0x0061U, 0x0301U}};
    const std::array<std::uint32_t, 1> greek{{0x03b1U}};
    const std::array<std::uint32_t, 1> cyrillic{{0x0410U}};
    const std::array<std::uint32_t, 1> arabic{{0x0627U}};
    const std::array<std::uint32_t, 1> hebrew{{0x05d0U}};
    const std::array<std::uint32_t, 2> devanagari{{0x0915U, 0x093fU}};
    const std::array<std::uint32_t, 1> han{{0x4e00U}};
    const std::array<std::uint32_t, 1> emoji{{0x1f600U}};
    const std::array<std::uint32_t, 1> missing{{0x10ffffU}};

    while (true) {
        bool complete = true;
        complete = append_cluster(&fixture, latin, scripts[0]) && complete;
        complete = append_cluster(&fixture, latin_mark, scripts[0]) && complete;
        complete = append_cluster(&fixture, greek, scripts[1]) && complete;
        complete = append_cluster(&fixture, cyrillic, scripts[2]) && complete;
        complete = append_cluster(&fixture, arabic, scripts[3]) && complete;
        complete = append_cluster(&fixture, hebrew, scripts[4]) && complete;
        complete = append_cluster(&fixture, devanagari, scripts[5]) && complete;
        complete = append_cluster(&fixture, han, scripts[6]) && complete;
        complete = append_cluster(&fixture, emoji, scripts[7]) && complete;
        complete = append_cluster(&fixture, missing, ScriptId::Zzzz) && complete;
        if (!complete) {
            break;
        }
    }

    while (fixture.source_bytes < kFixtureBytes) {
        if (!append_cluster(&fixture, latin, scripts[0])) {
            break;
        }
    }
    if (fixture.source_bytes != kFixtureBytes || fixture.script_runs.empty()) {
        std::cerr << "failed to construct exact 64 KiB fixture\n";
        std::exit(3);
    }
    fixture.script_runs.push_back(ScriptRunBoundary{
        fixture.source_bytes,
        static_cast<std::uint32_t>(fixture.graphemes.size() - 1U),
        fixture.script_runs.back().script,
        0U});
    return fixture;
}

std::vector<FontCoverageRange> coverage_for_face(
    std::size_t index,
    const std::array<ScriptId, 8>& scripts) {
    if (index == 0U) {
        return {{0x0020U, 0x007eU}};
    }
    if (index == 1U) {
        return {{0x0020U, 0x024fU}, {0x0300U, 0x036fU}};
    }
    const ScriptId script = scripts[(index - 2U) % scripts.size()];
    if (script == scripts[1]) {
        return {{0x0370U, 0x03ffU}};
    }
    if (script == scripts[2]) {
        return {{0x0400U, 0x052fU}};
    }
    if (script == scripts[3]) {
        return {{0x0600U, 0x06ffU}};
    }
    if (script == scripts[4]) {
        return {{0x0590U, 0x05ffU}};
    }
    if (script == scripts[5]) {
        return {{0x0900U, 0x097fU}};
    }
    if (script == scripts[6]) {
        return {{0x4e00U, 0x9fffU}};
    }
    if (script == scripts[7]) {
        return {{0x1f300U, 0x1faffU}};
    }
    return {{0xe000U, 0xe0ffU}};
}

double percentile(const std::vector<double>& sorted, double fraction) {
    const double position = fraction * static_cast<double>(sorted.size() - 1U);
    const std::size_t lower = static_cast<std::size_t>(position);
    const std::size_t upper = std::min(lower + 1U, sorted.size() - 1U);
    const double weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t iterations = argc > 1
        ? static_cast<std::size_t>(std::stoull(argv[1]))
        : 128U;
    const std::size_t fallback_hard_limit = argc > 2
        ? static_cast<std::size_t>(std::stoull(argv[2]))
        : 512U * 1024U;
    if (iterations == 0U) {
        std::cerr << "iterations must be positive\n";
        return 2;
    }

    const std::array<ScriptId, 8> scripts{{
        require_script("Latn"),
        require_script("Grek"),
        require_script("Cyrl"),
        require_script("Arab"),
        require_script("Hebr"),
        require_script("Deva"),
        require_script("Hani"),
        ScriptId::Zyyy,
    }};

    std::vector<std::vector<FontCoverageRange>> coverage_storage(kFaceCount);
    std::vector<FontFaceSeed> seeds;
    seeds.reserve(kFaceCount);
    for (std::size_t index = 0U; index < kFaceCount; ++index) {
        coverage_storage[index] = coverage_for_face(index, scripts);
        const ScriptId preferred_script = index == 0U || index == 1U
            ? scripts[0]
            : scripts[(index - 2U) % scripts.size()];
        seeds.push_back(FontFaceSeed{
            static_cast<std::uint64_t>(index + 1U),
            static_cast<std::uint32_t>(index + 1U),
            static_cast<std::uint16_t>(400U),
            static_cast<std::uint8_t>(5U),
            FontSlant::Upright,
            preferred_script,
            static_cast<std::uint16_t>(0U),
            std::span<const FontCoverageRange>(coverage_storage[index])});
    }

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::FontCatalog, 128U * 1024U);
    ledger.set_hard_limit(ResourceClass::FontFallbackPlan, fallback_hard_limit);
    LedgerMemoryResource catalog_resource(ledger, ResourceClass::FontCatalog);
    LedgerMemoryResource fallback_resource(ledger, ResourceClass::FontFallbackPlan);

    FontCatalog catalog(&catalog_resource);
    FontCatalogStats catalog_stats;
    FontCatalogError catalog_error;
    if (!build_font_catalog(seeds, &catalog, &catalog_stats, &catalog_error)) {
        std::cerr << "catalog build failed: " << catalog_error.message << '\n';
        return 4;
    }

    const Fixture fixture = make_fixture(scripts);
    const FontFaceId primary = font_face_id_by_stable_key(catalog, 1U);
    const FontFaceId complete_latin = font_face_id_by_stable_key(catalog, 2U);
    const std::array<FontFaceId, 1> preferred{{complete_latin}};
    const FontFallbackRequest request{
        primary,
        std::span<const FontFaceId>(preferred),
        {400U, 5U, FontSlant::Upright}};

    FontFallbackPlan plan(&fallback_resource);
    FontFallbackStats stats;
    FontFallbackError error;
    std::vector<double> durations_ms;
    durations_ms.reserve(iterations);

    for (std::size_t iteration = 0U;
         iteration < kWarmupIterations + iterations;
         ++iteration) {
        const auto begin = std::chrono::steady_clock::now();
        const bool ok = build_font_fallback_plan(
            fixture.codepoints,
            fixture.graphemes,
            fixture.script_runs,
            catalog,
            request,
            &plan,
            &stats,
            &error);
        const auto end = std::chrono::steady_clock::now();
        if (!ok) {
            std::cerr << "fallback failed: " << error.message << '\n';
            return 5;
        }
        if (iteration >= kWarmupIterations) {
            durations_ms.push_back(
                std::chrono::duration<double, std::milli>(end - begin).count());
        }
    }
    std::sort(durations_ms.begin(), durations_ms.end());

    const double p50 = percentile(durations_ms, 0.50);
    const double p95 = percentile(durations_ms, 0.95);
    const double p99 = percentile(durations_ms, 0.99);
    const double maximum = durations_ms.back();
    const double throughput = p50 == 0.0
        ? 0.0
        : 0.0625 / (p50 / 1000.0);
    const auto catalog_snapshot = ledger.snapshot(ResourceClass::FontCatalog);
    const auto fallback_snapshot = ledger.snapshot(ResourceClass::FontFallbackPlan);

    std::cout << std::fixed << std::setprecision(6)
              << "{\"schema\":\"zevryon.font-fallback-benchmark.v1\"," 
              << "\"fixture_bytes\":" << kFixtureBytes << ','
              << "\"iterations\":" << iterations << ','
              << "\"warmup_iterations\":" << kWarmupIterations << ','
              << "\"catalog_faces\":" << catalog.faces.size() << ','
              << "\"catalog_coverage_ranges\":" << catalog.coverage_ranges.size() << ','
              << "\"input_codepoints\":" << fixture.codepoints.size() << ','
              << "\"input_clusters\":" << fixture.graphemes.size() - 1U << ','
              << "\"script_runs\":" << fixture.script_runs.size() - 1U << ','
              << "\"output_runs\":" << stats.output_runs << ','
              << "\"output_boundaries\":" << plan.boundaries.size() << ','
              << "\"boundary_record_bytes\":" << sizeof(FontFallbackBoundary) << ','
              << "\"primary_clusters\":" << stats.primary_clusters << ','
              << "\"preferred_family_clusters\":" << stats.preferred_family_clusters << ','
              << "\"script_match_clusters\":" << stats.script_match_clusters << ','
              << "\"neutral_script_clusters\":" << stats.neutral_script_clusters << ','
              << "\"cross_script_clusters\":" << stats.cross_script_clusters << ','
              << "\"missing_clusters\":" << stats.missing_clusters << ','
              << "\"coverage_checks\":" << stats.coverage_checks << ','
              << "\"p50_ms\":" << p50 << ','
              << "\"p95_ms\":" << p95 << ','
              << "\"p99_ms\":" << p99 << ','
              << "\"maximum_ms\":" << maximum << ','
              << "\"p50_throughput_mib_s\":" << throughput << ','
              << "\"catalog_current_bytes\":" << catalog_snapshot.current_bytes << ','
              << "\"catalog_peak_bytes\":" << catalog_snapshot.peak_bytes << ','
              << "\"fallback_hard_limit_bytes\":" << fallback_hard_limit << ','
              << "\"fallback_current_bytes\":" << fallback_snapshot.current_bytes << ','
              << "\"fallback_peak_bytes\":" << fallback_snapshot.peak_bytes << ','
              << "\"rejected_reservations\":" << fallback_snapshot.rejected_reservations << ','
              << "\"accounting_errors\":" << fallback_snapshot.accounting_errors << ','
              << "\"within_hard_limits\":"
              << (ledger.within_hard_limits() ? "true" : "false") << ','
              << "\"accounting_clean\":"
              << (ledger.accounting_clean() ? "true" : "false")
              << "}\n";
    return 0;
}
