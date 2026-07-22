#include "grapheme_segmenter.hpp"
#include "ledger_memory_resource.hpp"
#include "unicode_grapheme_data.hpp"

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
        'Z', 'e', 'v', 'r', 'y', 'o', 'n', ' ',
        0x015fU, 0x011fU, 0x0131U, ' ',
        'e', 0x0301U, ' ',
        0x1f469U, 0x200dU, 0x1f680U, ' ',
        0x1f1f9U, 0x1f1f7U, ' ',
        0x0915U, 0x094dU, 0x0915U, ' ',
        0x1100U, 0x1161U, 0x11a8U, ' ',
        0x0600U, 0x0627U, ' ',
        0x000dU, 0x000aU,
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
    std::size_t cluster_budget_bytes = 1024U * 1024U;
    if ((argc > 1 && !parse_size(argv[1], &iterations)) ||
        (argc > 2 && !parse_size(argv[2], &cluster_budget_bytes)) ||
        argc > 3 || iterations < 10U || cluster_budget_bytes == 0U) {
        std::cerr << "usage: zevryon-grapheme-benchmark "
                     "[iterations>=10] [boundary-budget-bytes]\n";
        return 2;
    }

    constexpr std::size_t kFixtureBytes = 64U * 1024U;
    const auto codepoints = make_fixture(kFixtureBytes);
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(
        zevryon::core::ResourceClass::GraphemeCluster,
        cluster_budget_bytes);
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::GraphemeCluster);
    std::pmr::vector<zevryon::text::GraphemeBoundary> boundaries(&memory);
    std::vector<double> samples_ms;
    samples_ms.reserve(iterations);
    std::size_t expected_clusters = 0U;
    zevryon::text::GraphemeSegmentStats final_stats;

    for (std::size_t iteration = 0U;
         iteration < iterations + 32U;
         ++iteration) {
        zevryon::text::GraphemeSegmentStats stats;
        zevryon::text::GraphemeError error;
        const auto started = std::chrono::steady_clock::now();
        if (!zevryon::text::segment_graphemes(
                codepoints,
                &boundaries,
                &stats,
                &error)) {
            std::cerr << "segmentation failed: "
                      << zevryon::text::grapheme_error_kind_name(error.kind)
                      << " at " << error.codepoint_index << ' '
                      << error.message << '\n';
            return 1;
        }
        const auto ended = std::chrono::steady_clock::now();
        const std::size_t cluster_count =
            boundaries.empty() ? 0U : boundaries.size() - 1U;
        if (stats.input_codepoints != codepoints.size() ||
            stats.output_clusters != cluster_count ||
            boundaries.size() < 2U ||
            boundaries.front().codepoint_index != 0U ||
            boundaries.back().codepoint_index != codepoints.size()) {
            std::cerr << "grapheme statistics failed benchmark contract\n";
            return 1;
        }
        if (expected_clusters == 0U) {
            expected_clusters = cluster_count;
        } else if (cluster_count != expected_clusters) {
            std::cerr << "cluster count changed between iterations\n";
            return 1;
        }
        final_stats = stats;
        if (iteration >= 32U) {
            samples_ms.push_back(
                std::chrono::duration<double, std::milli>(ended - started).count());
        }
    }

    const auto resource =
        ledger.snapshot(zevryon::core::ResourceClass::GraphemeCluster);
    const double p50 = percentile(samples_ms, 50.0);
    const double p95 = percentile(samples_ms, 95.0);
    const double p99 = percentile(samples_ms, 99.0);
    const double maximum = *std::max_element(samples_ms.begin(), samples_ms.end());
    const double throughput_mib_per_second =
        (static_cast<double>(kFixtureBytes) / (1024.0 * 1024.0)) /
        (p50 / 1000.0);

    std::cout << '{'
              << "\"schema\":\"zevryon.grapheme-benchmark.v1\","
              << "\"unicode_version\":\""
              << zevryon::text::kUnicodeGraphemeDataVersion << "\","
              << "\"data_fingerprint\":\""
              << zevryon::text::kUnicodeGraphemeDataFingerprint << "\","
              << "\"boundary_record_bytes\":"
              << sizeof(zevryon::text::GraphemeBoundary) << ','
              << "\"fixture_bytes\":" << kFixtureBytes << ','
              << "\"iterations\":" << iterations << ','
              << "\"warmup_iterations\":32,"
              << "\"input_codepoints\":" << codepoints.size() << ','
              << "\"output_clusters\":" << expected_clusters << ','
              << "\"output_boundaries\":" << boundaries.size() << ','
              << "\"suppressed_breaks\":"
              << final_stats.suppressed_breaks << ','
              << "\"maximum_cluster_codepoints\":"
              << final_stats.maximum_cluster_codepoints << ','
              << "\"maximum_cluster_source_bytes\":"
              << final_stats.maximum_cluster_source_bytes << ','
              << "\"p50_ms\":" << p50 << ','
              << "\"p95_ms\":" << p95 << ','
              << "\"p99_ms\":" << p99 << ','
              << "\"maximum_ms\":" << maximum << ','
              << "\"p50_mib_per_second\":" << throughput_mib_per_second << ','
              << "\"cluster_hard_limit_bytes\":"
              << resource.hard_limit_bytes << ','
              << "\"cluster_current_bytes\":" << resource.current_bytes << ','
              << "\"cluster_peak_bytes\":" << resource.peak_bytes << ','
              << "\"rejected_reservations\":"
              << resource.rejected_reservations << ','
              << "\"accounting_errors\":" << resource.accounting_errors << ','
              << "\"within_hard_limits\":"
              << (ledger.within_hard_limits() ? "true" : "false") << ','
              << "\"accounting_clean\":"
              << (ledger.accounting_clean() ? "true" : "false")
              << "}\n";
    return 0;
}
