#include "bidi_explicit.hpp"
#include "bidi_sequence.hpp"
#include "bidi_weak.hpp"
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
        'a', '1', 0x0301U, ' ',
        0x0627U, '2', 0x0661U, ',', 0x0662U, ' ',
        0x05d0U, '3', '+', '4', '$', '$', ' ',
        0x2067U, 0x0628U, '5', 0x2066U, 'b', '6', 0x2069U,
        0x062aU, 0x2069U, 0x0301U, ' ',
        0x202eU, '7', '$', 0x202cU, ' ', '(', ')',
        ' ', 'a', '+', 'b', ',', 'c', '$', 'd',
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
    std::size_t weak_budget_bytes = 64U * 1024U;
    if ((argc > 1 && !parse_size(argv[1], &iterations)) ||
        (argc > 2 && !parse_size(argv[2], &weak_budget_bytes)) ||
        argc > 3 || iterations < 10U || weak_budget_bytes == 0U) {
        std::cerr << "usage: zevryon-bidi-weak-benchmark "
                     "[iterations>=10] [weak-budget-bytes]\n";
        return 2;
    }

    constexpr std::size_t kFixtureBytes = 64U * 1024U;
    const auto codepoints = make_fixture(kFixtureBytes);

    zevryon::core::ResourceLedger explicit_ledger;
    explicit_ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiRun,
        2U * 1024U * 1024U);
    zevryon::core::LedgerMemoryResource explicit_memory(
        explicit_ledger,
        zevryon::core::ResourceClass::BidiRun);
    std::pmr::vector<zevryon::text::BidiExplicitUnit> explicit_units(
        &explicit_memory);
    zevryon::text::BidiExplicitStats explicit_stats;
    zevryon::text::BidiExplicitError explicit_error;
    if (!zevryon::text::resolve_bidi_explicit(
            codepoints,
            zevryon::text::BidiParagraphDirection::Auto,
            &explicit_units,
            &explicit_stats,
            &explicit_error)) {
        std::cerr << "explicit fixture failed: " << explicit_error.message << '\n';
        return 1;
    }

    zevryon::core::ResourceLedger sequence_ledger;
    sequence_ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiSequence,
        2U * 1024U * 1024U);
    zevryon::core::LedgerMemoryResource sequence_memory(
        sequence_ledger,
        zevryon::core::ResourceClass::BidiSequence);
    zevryon::text::BidiSequenceTopology topology(&sequence_memory);
    zevryon::text::BidiSequenceStats sequence_stats;
    zevryon::text::BidiSequenceError sequence_error;
    if (!zevryon::text::build_bidi_isolating_run_sequences(
            explicit_units,
            explicit_stats.paragraph_level,
            &topology,
            &sequence_stats,
            &sequence_error)) {
        std::cerr << "sequence fixture failed: " << sequence_error.message << '\n';
        return 1;
    }

    zevryon::core::ResourceLedger weak_ledger;
    weak_ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiTypeResolution,
        weak_budget_bytes);
    zevryon::core::LedgerMemoryResource weak_memory(
        weak_ledger,
        zevryon::core::ResourceClass::BidiTypeResolution);
    std::pmr::vector<zevryon::text::BidiClass> types(&weak_memory);
    std::vector<double> samples_ms;
    samples_ms.reserve(iterations);
    zevryon::text::BidiWeakStats final_stats;
    std::size_t expected_types = 0U;

    for (std::size_t iteration = 0U; iteration < iterations + 32U; ++iteration) {
        zevryon::text::BidiWeakStats stats;
        zevryon::text::BidiWeakError error;
        const auto started = std::chrono::steady_clock::now();
        if (!zevryon::text::resolve_bidi_weak_types(
                explicit_units,
                topology,
                &types,
                &stats,
                &error)) {
            std::cerr << "weak-type resolution failed: "
                      << zevryon::text::bidi_weak_error_kind_name(error.kind)
                      << " at active index " << error.active_index << ' '
                      << error.message << '\n';
            return 1;
        }
        const auto ended = std::chrono::steady_clock::now();

        if (types.size() != topology.active_unit_indices.size() ||
            stats.output_types != types.size() ||
            stats.active_units != types.size() ||
            stats.isolating_sequences != topology.sequences.size()) {
            std::cerr << "weak benchmark output contract failed\n";
            return 1;
        }
        if (expected_types == 0U) {
            expected_types = types.size();
        } else if (types.size() != expected_types) {
            std::cerr << "weak output size changed between iterations\n";
            return 1;
        }
        final_stats = stats;
        if (iteration >= 32U) {
            samples_ms.push_back(
                std::chrono::duration<double, std::milli>(ended - started).count());
        }
    }

    const zevryon::core::ResourceSnapshot resource = weak_ledger.snapshot(
        zevryon::core::ResourceClass::BidiTypeResolution);
    const double p50 = percentile(samples_ms, 50.0);
    const double p95 = percentile(samples_ms, 95.0);
    const double p99 = percentile(samples_ms, 99.0);
    const double maximum = *std::max_element(samples_ms.begin(), samples_ms.end());
    const double throughput_mib_per_second =
        (static_cast<double>(kFixtureBytes) / (1024.0 * 1024.0)) /
        (p50 / 1000.0);

    std::cout << '{'
              << "\"schema\":\"zevryon.bidi-weak-benchmark.v1\","
              << "\"fixture_bytes\":" << kFixtureBytes << ','
              << "\"iterations\":" << iterations << ','
              << "\"warmup_iterations\":32,"
              << "\"input_codepoints\":" << codepoints.size() << ','
              << "\"input_units\":" << explicit_units.size() << ','
              << "\"active_units\":" << topology.active_unit_indices.size() << ','
              << "\"isolating_sequences\":" << topology.sequences.size() << ','
              << "\"output_types\":" << final_stats.output_types << ','
              << "\"type_record_bytes\":"
              << sizeof(zevryon::text::BidiClass) << ','
              << "\"w1_changes\":" << final_stats.w1_nsm_changes << ','
              << "\"w2_changes\":" << final_stats.w2_en_to_an << ','
              << "\"w3_changes\":" << final_stats.w3_al_to_r << ','
              << "\"w4_changes\":" << final_stats.w4_separator_changes << ','
              << "\"w5_changes\":" << final_stats.w5_et_to_en << ','
              << "\"w6_changes\":" << final_stats.w6_neutralized << ','
              << "\"w7_changes\":" << final_stats.w7_en_to_l << ','
              << "\"p50_ms\":" << p50 << ','
              << "\"p95_ms\":" << p95 << ','
              << "\"p99_ms\":" << p99 << ','
              << "\"maximum_ms\":" << maximum << ','
              << "\"p50_mib_per_second\":" << throughput_mib_per_second << ','
              << "\"weak_hard_limit_bytes\":" << resource.hard_limit_bytes << ','
              << "\"weak_current_bytes\":" << resource.current_bytes << ','
              << "\"weak_peak_bytes\":" << resource.peak_bytes << ','
              << "\"rejected_reservations\":"
              << resource.rejected_reservations << ','
              << "\"accounting_errors\":" << resource.accounting_errors << ','
              << "\"within_hard_limits\":"
              << (weak_ledger.within_hard_limits() ? "true" : "false") << ','
              << "\"accounting_clean\":"
              << (weak_ledger.accounting_clean() ? "true" : "false")
              << "}\n";
    return 0;
}
