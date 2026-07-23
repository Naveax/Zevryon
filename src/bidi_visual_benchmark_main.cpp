#include "bidi_explicit.hpp"
#include "bidi_implicit.hpp"
#include "bidi_neutral.hpp"
#include "bidi_sequence.hpp"
#include "bidi_visual.hpp"
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
        'a', 0x05D0U, 0x0301U, '1', 0x0661U, ' ',
        0x2067U, 0x05D1U, 'b', 0x2069U, ' ',
        0x202BU, 'c', 0x05D2U, '3', 0x202CU, ' ',
        'd', '(', 0x05D3U, ')', 0x0301U, ' ',
        0x0627U, '4', ',', '5', '$', ' ',
        0x202BU, 0x05D4U, ' ', 0x0009U, 0x202CU,
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
    std::size_t visual_budget_bytes = 256U * 1024U;
    if ((argc > 1 && !parse_size(argv[1], &iterations)) ||
        (argc > 2 && !parse_size(argv[2], &visual_budget_bytes)) ||
        argc > 3 || iterations < 10U || visual_budget_bytes == 0U) {
        std::cerr << "usage: zevryon-bidi-visual-benchmark "
                     "[iterations>=10] [visual-budget-bytes]\n";
        return 2;
    }

    constexpr std::size_t kFixtureBytes = 64U * 1024U;
    constexpr std::size_t kActiveUnitsPerFullLine = 30U;
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
        128U * 1024U);
    zevryon::core::LedgerMemoryResource weak_memory(
        weak_ledger,
        zevryon::core::ResourceClass::BidiTypeResolution);
    std::pmr::vector<zevryon::text::BidiClass> weak_types(&weak_memory);
    zevryon::text::BidiWeakStats weak_stats;
    zevryon::text::BidiWeakError weak_error;
    if (!zevryon::text::resolve_bidi_weak_types(
            explicit_units,
            topology,
            &weak_types,
            &weak_stats,
            &weak_error)) {
        std::cerr << "weak fixture failed: " << weak_error.message << '\n';
        return 1;
    }

    zevryon::core::ResourceLedger neutral_ledger;
    neutral_ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiNeutralResolution,
        128U * 1024U);
    zevryon::core::LedgerMemoryResource neutral_memory(
        neutral_ledger,
        zevryon::core::ResourceClass::BidiNeutralResolution);
    std::pmr::vector<zevryon::text::BidiClass> neutral_types(&neutral_memory);
    zevryon::text::BidiNeutralStats neutral_stats;
    zevryon::text::BidiNeutralError neutral_error;
    if (!zevryon::text::resolve_bidi_neutral_types(
            codepoints,
            explicit_units,
            topology,
            weak_types,
            &neutral_types,
            &neutral_stats,
            &neutral_error)) {
        std::cerr << "neutral fixture failed: " << neutral_error.message << '\n';
        return 1;
    }

    zevryon::core::ResourceLedger implicit_ledger;
    implicit_ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiImplicitLevel,
        64U * 1024U);
    zevryon::core::LedgerMemoryResource implicit_memory(
        implicit_ledger,
        zevryon::core::ResourceClass::BidiImplicitLevel);
    std::pmr::vector<std::uint8_t> implicit_levels(&implicit_memory);
    zevryon::text::BidiImplicitStats implicit_stats;
    zevryon::text::BidiImplicitError implicit_error;
    if (!zevryon::text::resolve_bidi_implicit_levels(
            explicit_units,
            topology,
            neutral_types,
            &implicit_levels,
            &implicit_stats,
            &implicit_error)) {
        std::cerr << "implicit fixture failed: " << implicit_error.message << '\n';
        return 1;
    }

    std::vector<zevryon::text::BidiLineSpan> lines;
    for (std::size_t first = 0U; first < implicit_levels.size();) {
        const std::size_t count = std::min(
            kActiveUnitsPerFullLine,
            implicit_levels.size() - first);
        lines.push_back(zevryon::text::BidiLineSpan{
            static_cast<std::uint32_t>(first),
            static_cast<std::uint32_t>(count)});
        first += count;
    }

    zevryon::core::ResourceLedger visual_ledger;
    visual_ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiVisualOrder,
        visual_budget_bytes);
    zevryon::core::LedgerMemoryResource visual_memory(
        visual_ledger,
        zevryon::core::ResourceClass::BidiVisualOrder);
    zevryon::text::BidiVisualOrder visual(&visual_memory);
    std::vector<double> samples_ms;
    samples_ms.reserve(iterations);
    zevryon::text::BidiVisualStats final_stats;
    std::size_t expected_output = 0U;

    for (std::size_t iteration = 0U; iteration < iterations + 32U; ++iteration) {
        zevryon::text::BidiVisualStats stats;
        zevryon::text::BidiVisualError error;
        const auto started = std::chrono::steady_clock::now();
        if (!zevryon::text::resolve_bidi_visual_order(
                explicit_units,
                topology,
                implicit_levels,
                explicit_stats.paragraph_level,
                lines,
                &visual,
                &stats,
                &error)) {
            std::cerr << "visual resolution failed: "
                      << zevryon::text::bidi_visual_error_kind_name(error.kind)
                      << " at active index " << error.active_index << ' '
                      << error.message << '\n';
            return 1;
        }
        const auto ended = std::chrono::steady_clock::now();
        if (visual.line_levels.size() != implicit_levels.size() ||
            visual.visual_to_active.size() != implicit_levels.size() ||
            stats.output_levels != implicit_levels.size() ||
            stats.output_visual_indices != implicit_levels.size() ||
            stats.lines != lines.size()) {
            std::cerr << "visual benchmark output contract failed\n";
            return 1;
        }
        if (expected_output == 0U) {
            expected_output = visual.visual_to_active.size();
        } else if (visual.visual_to_active.size() != expected_output) {
            std::cerr << "visual output size changed between iterations\n";
            return 1;
        }
        final_stats = stats;
        if (iteration >= 32U) {
            samples_ms.push_back(
                std::chrono::duration<double, std::milli>(ended - started).count());
        }
    }

    const auto resource = visual_ledger.snapshot(
        zevryon::core::ResourceClass::BidiVisualOrder);
    const double p50 = percentile(samples_ms, 50.0);
    const double p95 = percentile(samples_ms, 95.0);
    const double p99 = percentile(samples_ms, 99.0);
    const double maximum = *std::max_element(samples_ms.begin(), samples_ms.end());
    const double throughput_mib_per_second =
        (static_cast<double>(kFixtureBytes) / (1024.0 * 1024.0)) /
        (p50 / 1000.0);

    std::cout << '{'
              << "\"schema\":\"zevryon.bidi-visual-benchmark.v1\","
              << "\"uax_version\":\"Unicode 17.0.0 / UAX #9 revision 51\","
              << "\"fixture_bytes\":" << kFixtureBytes << ','
              << "\"iterations\":" << iterations << ','
              << "\"warmup_iterations\":32,"
              << "\"input_codepoints\":" << codepoints.size() << ','
              << "\"input_units\":" << explicit_units.size() << ','
              << "\"active_units\":" << topology.active_unit_indices.size() << ','
              << "\"lines\":" << lines.size() << ','
              << "\"output_levels\":" << final_stats.output_levels << ','
              << "\"output_visual_indices\":"
              << final_stats.output_visual_indices << ','
              << "\"level_record_bytes\":1,"
              << "\"visual_index_record_bytes\":4,"
              << "\"l1_separator_resets\":"
              << final_stats.l1_separator_resets << ','
              << "\"l1_whitespace_resets\":"
              << final_stats.l1_whitespace_resets << ','
              << "\"l1_isolate_resets\":"
              << final_stats.l1_isolate_resets << ','
              << "\"l2_reversal_spans\":"
              << final_stats.l2_reversal_spans << ','
              << "\"l2_reversed_units\":"
              << final_stats.l2_reversed_units << ','
              << "\"l3_combining_sequences\":"
              << final_stats.l3_combining_sequences << ','
              << "\"l3_repaired_units\":"
              << final_stats.l3_repaired_units << ','
              << "\"maximum_input_level\":"
              << static_cast<unsigned>(final_stats.maximum_input_level) << ','
              << "\"maximum_line_level\":"
              << static_cast<unsigned>(final_stats.maximum_line_level) << ','
              << "\"p50_ms\":" << p50 << ','
              << "\"p95_ms\":" << p95 << ','
              << "\"p99_ms\":" << p99 << ','
              << "\"maximum_ms\":" << maximum << ','
              << "\"p50_mib_per_second\":" << throughput_mib_per_second << ','
              << "\"visual_hard_limit_bytes\":" << resource.hard_limit_bytes << ','
              << "\"visual_current_bytes\":" << resource.current_bytes << ','
              << "\"visual_peak_bytes\":" << resource.peak_bytes << ','
              << "\"rejected_reservations\":"
              << resource.rejected_reservations << ','
              << "\"accounting_errors\":" << resource.accounting_errors << ','
              << "\"within_hard_limits\":"
              << (visual_ledger.within_hard_limits() ? "true" : "false") << ','
              << "\"accounting_clean\":"
              << (visual_ledger.accounting_clean() ? "true" : "false")
              << "}\n";
    return 0;
}
