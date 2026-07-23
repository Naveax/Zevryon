#include "bidi_explicit.hpp"
#include "bidi_sequence.hpp"
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
        'a', 'b', ' ', 0x202bU, 0x05d0U, 0x05d1U, 0x202aU, 'c',
        'd', 0x202cU, 0x05d2U, 0x202cU, ' ',
        0x2067U, 0x0627U, 0x0628U, 0x2066U, 'e', 'f', 0x2069U,
        0x062aU, 0x2069U, ' ', 0x2068U, 0x05d3U, 0x2069U, ' ',
        0x202eU, '1', '2', 0x202cU, ' ', 0x00adU, 0x0301U,
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
    std::size_t sequence_budget_bytes = 4U * 1024U * 1024U;
    if ((argc > 1 && !parse_size(argv[1], &iterations)) ||
        (argc > 2 && !parse_size(argv[2], &sequence_budget_bytes)) ||
        argc > 3 || iterations < 10U || sequence_budget_bytes == 0U) {
        std::cerr << "usage: zevryon-bidi-sequence-benchmark "
                     "[iterations>=10] [sequence-budget-bytes]\n";
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
            zevryon::text::BidiParagraphDirection::Left,
            &explicit_units,
            &explicit_stats,
            &explicit_error)) {
        std::cerr << "explicit fixture failed: " << explicit_error.message << '\n';
        return 1;
    }

    zevryon::core::ResourceLedger sequence_ledger;
    sequence_ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiSequence,
        sequence_budget_bytes);
    zevryon::core::LedgerMemoryResource sequence_memory(
        sequence_ledger,
        zevryon::core::ResourceClass::BidiSequence);
    zevryon::text::BidiSequenceTopology topology(&sequence_memory);
    std::vector<double> samples_ms;
    samples_ms.reserve(iterations);
    zevryon::text::BidiSequenceStats final_stats;
    std::size_t expected_active = 0U;
    std::size_t expected_runs = 0U;
    std::size_t expected_sequences = 0U;

    for (std::size_t iteration = 0U; iteration < iterations + 32U; ++iteration) {
        zevryon::text::BidiSequenceStats stats;
        zevryon::text::BidiSequenceError error;
        const auto started = std::chrono::steady_clock::now();
        if (!zevryon::text::build_bidi_isolating_run_sequences(
                explicit_units,
                explicit_stats.paragraph_level,
                &topology,
                &stats,
                &error)) {
            std::cerr << "sequence construction failed: "
                      << zevryon::text::bidi_sequence_error_kind_name(error.kind)
                      << " at unit " << error.unit_index << ' '
                      << error.message << '\n';
            return 1;
        }
        const auto ended = std::chrono::steady_clock::now();

        std::uint64_t covered_active = 0U;
        for (const zevryon::text::BidiLevelRun& run : topology.level_runs) {
            covered_active += run.active_count;
        }
        if (covered_active != topology.active_unit_indices.size() ||
            topology.sequence_run_indices.size() != topology.level_runs.size() ||
            topology.sequences.size() != stats.isolating_sequences) {
            std::cerr << "sequence benchmark topology contract failed\n";
            return 1;
        }

        if (expected_active == 0U) {
            expected_active = topology.active_unit_indices.size();
            expected_runs = topology.level_runs.size();
            expected_sequences = topology.sequences.size();
        } else if (topology.active_unit_indices.size() != expected_active ||
                   topology.level_runs.size() != expected_runs ||
                   topology.sequences.size() != expected_sequences) {
            std::cerr << "sequence topology changed between iterations\n";
            return 1;
        }
        final_stats = stats;
        if (iteration >= 32U) {
            samples_ms.push_back(
                std::chrono::duration<double, std::milli>(ended - started).count());
        }
    }

    const zevryon::core::ResourceSnapshot resource = sequence_ledger.snapshot(
        zevryon::core::ResourceClass::BidiSequence);
    const double p50 = percentile(samples_ms, 50.0);
    const double p95 = percentile(samples_ms, 95.0);
    const double p99 = percentile(samples_ms, 99.0);
    const double maximum = *std::max_element(samples_ms.begin(), samples_ms.end());
    const double throughput_mib_per_second =
        (static_cast<double>(kFixtureBytes) / (1024.0 * 1024.0)) /
        (p50 / 1000.0);

    std::cout << '{'
              << "\"schema\":\"zevryon.bidi-sequence-benchmark.v1\","
              << "\"fixture_bytes\":" << kFixtureBytes << ','
              << "\"iterations\":" << iterations << ','
              << "\"warmup_iterations\":32,"
              << "\"input_codepoints\":" << codepoints.size() << ','
              << "\"input_units\":" << explicit_units.size() << ','
              << "\"active_units\":" << final_stats.active_units << ','
              << "\"removed_units\":" << final_stats.removed_units << ','
              << "\"level_runs\":" << final_stats.level_runs << ','
              << "\"isolating_sequences\":" << final_stats.isolating_sequences << ','
              << "\"sequence_run_links\":" << final_stats.sequence_run_links << ','
              << "\"matched_isolates\":" << final_stats.matched_isolates << ','
              << "\"unmatched_isolate_initiators\":"
              << final_stats.unmatched_isolate_initiators << ','
              << "\"unmatched_pdi\":" << final_stats.unmatched_pdi << ','
              << "\"maximum_sequence_runs\":"
              << final_stats.maximum_sequence_runs << ','
              << "\"maximum_sequence_units\":"
              << final_stats.maximum_sequence_units << ','
              << "\"level_run_record_bytes\":"
              << sizeof(zevryon::text::BidiLevelRun) << ','
              << "\"sequence_record_bytes\":"
              << sizeof(zevryon::text::BidiIsolatingRunSequence) << ','
              << "\"p50_ms\":" << p50 << ','
              << "\"p95_ms\":" << p95 << ','
              << "\"p99_ms\":" << p99 << ','
              << "\"maximum_ms\":" << maximum << ','
              << "\"p50_mib_per_second\":" << throughput_mib_per_second << ','
              << "\"sequence_hard_limit_bytes\":"
              << resource.hard_limit_bytes << ','
              << "\"sequence_current_bytes\":" << resource.current_bytes << ','
              << "\"sequence_peak_bytes\":" << resource.peak_bytes << ','
              << "\"rejected_reservations\":"
              << resource.rejected_reservations << ','
              << "\"accounting_errors\":" << resource.accounting_errors << ','
              << "\"within_hard_limits\":"
              << (sequence_ledger.within_hard_limits() ? "true" : "false") << ','
              << "\"accounting_clean\":"
              << (sequence_ledger.accounting_clean() ? "true" : "false")
              << "}\n";
    return 0;
}
