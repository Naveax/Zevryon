#include "grapheme_segmenter.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"
#include "script_run.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <vector>

namespace {

bool parse_size(const char* text, std::size_t* value) {
    if (text == nullptr || value == nullptr) {
        return false;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' ||
        parsed > static_cast<unsigned long long>(
                     std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    *value = static_cast<std::size_t>(parsed);
    return true;
}

std::uint8_t utf8_length(std::uint32_t value) noexcept {
    return value <= 0x7fU ? 1U
         : value <= 0x7ffU ? 2U
         : value <= 0xffffU ? 3U
         : 4U;
}

std::vector<zevryon::text::DecodedCodePoint> make_fixture(
    std::size_t target_source_bytes) {
    const std::vector<std::uint32_t> pattern{
        'Z', 'e', 'v', 'r', 'y', 'o', 'n', ' ', '1', '2', ',', ' ',
        0x03b1U, 0x03b2U, 0x03b3U, ' ',
        0x0430U, 0x0431U, 0x0432U, ' ',
        0x0627U, 0x0628U, 0x062aU, ' ',
        0x4e00U, 0x4e8cU, 0x4e09U, ' ',
        0x3042U, 0x30fcU, 0x3044U, ' ',
        0x30a2U, 0x30fcU, 0x30a4U, ' ',
        0x0915U, 0x094dU, 0x0915U, ' ',
        'e', 0x0301U, ' ',
        0x1f469U, 0x200dU, 0x1f680U, ' ',
    };
    std::vector<zevryon::text::DecodedCodePoint> output;
    output.reserve(target_source_bytes / 2U);
    std::uint64_t source = 0U;
    std::size_t pattern_index = 0U;
    while (source < target_source_bytes) {
        std::uint32_t value = pattern[pattern_index];
        std::uint8_t length = utf8_length(value);
        if (source + length > target_source_bytes) {
            value = static_cast<std::uint32_t>('x');
            length = 1U;
        }
        output.emplace_back(value, source, source + length, false);
        source += length;
        pattern_index = (pattern_index + 1U) % pattern.size();
    }
    return output;
}

double percentile(std::vector<double> values, double percentage) {
    std::sort(values.begin(), values.end());
    const double position =
        static_cast<double>(values.size() - 1U) * percentage / 100.0;
    const std::size_t lower = static_cast<std::size_t>(position);
    const std::size_t upper = std::min(lower + 1U, values.size() - 1U);
    const double fraction = position - static_cast<double>(lower);
    return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

} // namespace

int main(int argc, char** argv) {
    std::size_t iterations = 1024U;
    std::size_t run_budget_bytes = 1U * 1024U * 1024U;
    if ((argc > 1 && !parse_size(argv[1], &iterations)) ||
        (argc > 2 && !parse_size(argv[2], &run_budget_bytes)) ||
        argc > 3 || iterations < 10U || run_budget_bytes == 0U) {
        std::cerr << "usage: zevryon-script-run-benchmark "
                     "[iterations>=10] [run-budget-bytes]\n";
        return 2;
    }

    constexpr std::size_t kFixtureBytes = 64U * 1024U;
    const auto codepoints = make_fixture(kFixtureBytes);

    zevryon::core::ResourceLedger grapheme_ledger;
    grapheme_ledger.set_hard_limit(
        zevryon::core::ResourceClass::GraphemeCluster,
        2U * 1024U * 1024U);
    zevryon::core::LedgerMemoryResource grapheme_memory(
        grapheme_ledger,
        zevryon::core::ResourceClass::GraphemeCluster);
    std::pmr::vector<zevryon::text::GraphemeBoundary> graphemes(
        &grapheme_memory);
    zevryon::text::GraphemeSegmentStats grapheme_stats;
    zevryon::text::GraphemeError grapheme_error;
    if (!zevryon::text::segment_graphemes(
            codepoints,
            &graphemes,
            &grapheme_stats,
            &grapheme_error)) {
        std::cerr << "fixture grapheme segmentation failed: "
                  << grapheme_error.message << '\n';
        return 1;
    }

    zevryon::core::ResourceLedger run_ledger;
    run_ledger.set_hard_limit(
        zevryon::core::ResourceClass::ScriptRun,
        run_budget_bytes);
    zevryon::core::LedgerMemoryResource run_memory(
        run_ledger,
        zevryon::core::ResourceClass::ScriptRun);
    std::pmr::vector<zevryon::text::ScriptRunBoundary> runs(&run_memory);
    std::vector<double> samples_ms;
    samples_ms.reserve(iterations);
    std::size_t expected_boundaries = 0U;
    zevryon::text::ScriptRunStats final_stats;

    for (std::size_t iteration = 0U; iteration < iterations + 32U; ++iteration) {
        zevryon::text::ScriptRunStats stats;
        zevryon::text::ScriptRunError error;
        const auto started = std::chrono::steady_clock::now();
        if (!zevryon::text::resolve_script_runs(
                codepoints,
                graphemes,
                &runs,
                &stats,
                &error)) {
            std::cerr << "script-run resolution failed: "
                      << zevryon::text::script_run_error_kind_name(error.kind)
                      << " at cluster " << error.cluster_index << ' '
                      << error.message << '\n';
            return 1;
        }
        const auto ended = std::chrono::steady_clock::now();
        if (runs.empty() || runs.size() != stats.output_runs + 1U ||
            stats.input_codepoints != codepoints.size() ||
            stats.input_clusters != grapheme_stats.output_clusters) {
            std::cerr << "script-run benchmark contract failed\n";
            return 1;
        }
        if (expected_boundaries == 0U) {
            expected_boundaries = runs.size();
        } else if (runs.size() != expected_boundaries) {
            std::cerr << "script-run boundary count changed between iterations\n";
            return 1;
        }
        final_stats = stats;
        if (iteration >= 32U) {
            samples_ms.push_back(
                std::chrono::duration<double, std::milli>(ended - started).count());
        }
    }

    const auto resource =
        run_ledger.snapshot(zevryon::core::ResourceClass::ScriptRun);
    const double p50 = percentile(samples_ms, 50.0);
    const double p95 = percentile(samples_ms, 95.0);
    const double p99 = percentile(samples_ms, 99.0);
    const double maximum = *std::max_element(samples_ms.begin(), samples_ms.end());
    const double throughput_mib_per_second =
        (static_cast<double>(kFixtureBytes) / (1024.0 * 1024.0)) /
        (p50 / 1000.0);

    std::cout << '{'
              << "\"schema\":\"zevryon.script-run-benchmark.v1\","
              << "\"unicode_version\":\""
              << zevryon::text::kUnicodeScriptDataVersion << "\","
              << "\"data_fingerprint\":\""
              << zevryon::text::kUnicodeScriptDataFingerprint << "\","
              << "\"fixture_bytes\":" << kFixtureBytes << ','
              << "\"iterations\":" << iterations << ','
              << "\"warmup_iterations\":32,"
              << "\"input_codepoints\":" << codepoints.size() << ','
              << "\"input_clusters\":" << final_stats.input_clusters << ','
              << "\"output_runs\":" << final_stats.output_runs << ','
              << "\"output_boundaries\":" << expected_boundaries << ','
              << "\"boundary_record_bytes\":"
              << sizeof(zevryon::text::ScriptRunBoundary) << ','
              << "\"neutral_clusters\":" << final_stats.neutral_clusters << ','
              << "\"explicit_extension_lookups\":"
              << final_stats.explicit_extension_lookups << ','
              << "\"internal_cluster_conflicts\":"
              << final_stats.internal_cluster_conflicts << ','
              << "\"maximum_run_clusters\":"
              << final_stats.maximum_run_clusters << ','
              << "\"p50_ms\":" << p50 << ','
              << "\"p95_ms\":" << p95 << ','
              << "\"p99_ms\":" << p99 << ','
              << "\"maximum_ms\":" << maximum << ','
              << "\"p50_mib_per_second\":" << throughput_mib_per_second << ','
              << "\"script_run_hard_limit_bytes\":"
              << resource.hard_limit_bytes << ','
              << "\"script_run_current_bytes\":" << resource.current_bytes << ','
              << "\"script_run_peak_bytes\":" << resource.peak_bytes << ','
              << "\"rejected_reservations\":"
              << resource.rejected_reservations << ','
              << "\"accounting_errors\":" << resource.accounting_errors << ','
              << "\"within_hard_limits\":"
              << (run_ledger.within_hard_limits() ? "true" : "false") << ','
              << "\"accounting_clean\":"
              << (run_ledger.accounting_clean() ? "true" : "false")
              << "}\n";
    return 0;
}
