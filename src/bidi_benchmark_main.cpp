#include "bidi_resolver.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

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
        'Z', 'e', 'v', 'r', 'y', 'o', 'n', ' ', '1', '2', '3', ' ',
        '(', 0x05d0U, 0x05d1U, 0x05d2U, ')', ' ',
        0x2067U, 0x0627U, 0x0644U, 0x0639U, 0x0631U, 0x0628U,
        0x064aU, 0x0629U, 0x2069U, ' ',
        '[', '4', '5', ',', '6', '7', ']', ' ',
        0x2066U, 'L', 'T', 'R', 0x2069U, ' ',
        0x202eU, 'a', 'b', 'c', 0x202cU, ' ',
        0x1f469U, 0x200dU, 0x1f680U, ' ',
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
    std::size_t budget_bytes = 8U * 1024U * 1024U;
    if ((argc > 1 && !parse_size(argv[1], &iterations)) ||
        (argc > 2 && !parse_size(argv[2], &budget_bytes)) ||
        argc > 3 || iterations < 10U || budget_bytes == 0U) {
        std::cerr << "usage: zevryon-bidi-benchmark "
                     "[iterations>=10] [bidi-budget-bytes]\n";
        return 2;
    }

    constexpr std::size_t kFixtureBytes = 64U * 1024U;
    const auto codepoints = make_fixture(kFixtureBytes);
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiBuffer,
        budget_bytes);
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiBuffer);
    std::pmr::vector<std::uint8_t> levels(&memory);
    std::pmr::vector<std::uint32_t> order(&memory);
    std::vector<double> samples_ms;
    samples_ms.reserve(iterations);
    std::size_t expected_order_size = 0U;
    zevryon::text::BidiStats final_stats;

    for (std::size_t iteration = 0U; iteration < iterations + 32U; ++iteration) {
        zevryon::text::BidiStats stats;
        zevryon::text::BidiError error;
        const auto started = std::chrono::steady_clock::now();
        if (!zevryon::text::resolve_bidi_codepoints(
                codepoints,
                zevryon::text::ParagraphDirection::Auto,
                &levels,
                &order,
                &stats,
                &error)) {
            std::cerr << "bidi benchmark resolution failed: "
                      << zevryon::text::bidi_error_kind_name(error.kind)
                      << " at " << error.input_index << ' '
                      << error.message << '\n';
            return 1;
        }
        const auto ended = std::chrono::steady_clock::now();
        if (levels.size() != codepoints.size() || order.empty() ||
            stats.input_units != codepoints.size()) {
            std::cerr << "bidi benchmark output contract failed\n";
            return 1;
        }
        if (expected_order_size == 0U) {
            expected_order_size = order.size();
        } else if (order.size() != expected_order_size) {
            std::cerr << "bidi visual order changed between iterations\n";
            return 1;
        }
        final_stats = stats;
        if (iteration >= 32U) {
            samples_ms.push_back(
                std::chrono::duration<double, std::milli>(ended - started).count());
        }
    }

    const auto resource =
        ledger.snapshot(zevryon::core::ResourceClass::BidiBuffer);
    const double p50 = percentile(samples_ms, 50.0);
    const double p95 = percentile(samples_ms, 95.0);
    const double p99 = percentile(samples_ms, 99.0);
    const double maximum = *std::max_element(samples_ms.begin(), samples_ms.end());
    const double throughput_mib_per_second =
        (static_cast<double>(kFixtureBytes) / (1024.0 * 1024.0)) /
        (p50 / 1000.0);

    std::cout << '{'
              << "\"schema\":\"zevryon.bidi-benchmark.v1\","
              << "\"unicode_version\":\""
              << zevryon::text::kUnicodeBidiDataVersion << "\","
              << "\"data_fingerprint\":\""
              << zevryon::text::kUnicodeBidiDataFingerprint << "\","
              << "\"fixture_bytes\":" << kFixtureBytes << ','
              << "\"iterations\":" << iterations << ','
              << "\"warmup_iterations\":32,"
              << "\"input_units\":" << codepoints.size() << ','
              << "\"visual_units\":" << expected_order_size << ','
              << "\"removed_units\":" << final_stats.removed_units << ','
              << "\"level_runs\":" << final_stats.level_runs << ','
              << "\"isolating_run_sequences\":"
              << final_stats.isolating_run_sequences << ','
              << "\"bracket_pairs\":" << final_stats.bracket_pairs << ','
              << "\"maximum_sequence_units\":"
              << final_stats.maximum_sequence_units << ','
              << "\"maximum_resolved_level\":"
              << static_cast<unsigned int>(final_stats.maximum_resolved_level) << ','
              << "\"p50_ms\":" << p50 << ','
              << "\"p95_ms\":" << p95 << ','
              << "\"p99_ms\":" << p99 << ','
              << "\"maximum_ms\":" << maximum << ','
              << "\"p50_mib_per_second\":" << throughput_mib_per_second << ','
              << "\"bidi_hard_limit_bytes\":" << resource.hard_limit_bytes << ','
              << "\"bidi_current_bytes\":" << resource.current_bytes << ','
              << "\"bidi_peak_bytes\":" << resource.peak_bytes << ','
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
