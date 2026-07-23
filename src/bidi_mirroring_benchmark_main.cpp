#include "bidi_mirroring.hpp"
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

struct PatternEntry {
    std::uint32_t codepoint;
    std::uint8_t level;
};

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
    std::size_t request_budget = 128U * 1024U;
    if ((argc > 1 && !parse_size(argv[1], &iterations)) ||
        (argc > 2 && !parse_size(argv[2], &request_budget)) ||
        argc > 3 || iterations < 10U || request_budget == 0U) {
        std::cerr << "usage: zevryon-bidi-mirroring-benchmark "
                     "[iterations>=10] [request-budget-bytes]\n";
        return 2;
    }

    constexpr std::size_t kFixtureBytes = 64U * 1024U;
    constexpr std::size_t kVisualBlock = 32U;
    const std::vector<PatternEntry> pattern{
        {0x28U, 1U},    // exact character mapping
        {0x61U, 1U},
        {0x62U, 0U},
        {0x2209U, 1U},  // best-fit character mapping
        {0x63U, 1U},
        {0x64U, 0U},
        {0x221BU, 1U},  // mirrored glyph only
        {0x65U, 1U},
        {0xFD3EU, 1U},  // legacy non-mirrored punctuation
        {0x29U, 0U},    // mirrored property, even level
        {0x66U, 1U},
        {0x67U, 0U},
        {0x68U, 1U},
        {0x69U, 0U},
        {0x6AU, 1U},
        {0x6BU, 0U},
    };

    std::vector<zevryon::text::DecodedCodePoint> codepoints;
    std::vector<zevryon::text::BidiExplicitUnit> units;
    zevryon::text::BidiSequenceTopology topology;
    zevryon::text::BidiVisualOrder visual;
    std::uint64_t source_offset = 0U;
    std::size_t pattern_index = 0U;
    while (source_offset < kFixtureBytes) {
        PatternEntry entry = pattern[pattern_index];
        std::uint8_t length = utf8_length(entry.codepoint);
        if (source_offset + length > kFixtureBytes) {
            entry = PatternEntry{0x78U, 0U};
            length = 1U;
        }
        const std::uint32_t index = static_cast<std::uint32_t>(codepoints.size());
        codepoints.emplace_back(
            entry.codepoint,
            source_offset,
            source_offset + length,
            false);
        units.push_back(zevryon::text::BidiExplicitUnit{
            source_offset,
            index,
            zevryon::text::BidiClass::ON,
            zevryon::text::BidiClass::ON,
            entry.level,
            0U});
        topology.active_unit_indices.push_back(index);
        visual.line_levels.push_back(entry.level);
        source_offset += length;
        pattern_index = (pattern_index + 1U) % pattern.size();
    }

    visual.visual_to_active.resize(codepoints.size());
    for (std::size_t first = 0U; first < codepoints.size(); first += kVisualBlock) {
        const std::size_t end = std::min(first + kVisualBlock, codepoints.size());
        for (std::size_t position = first; position < end; ++position) {
            visual.visual_to_active[position] = static_cast<std::uint32_t>(
                end - 1U - (position - first));
        }
    }

    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiMirrorRequest,
        request_budget);
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiMirrorRequest);
    zevryon::text::BidiMirrorRequests output(&memory);
    std::vector<double> samples_ms;
    samples_ms.reserve(iterations);
    zevryon::text::BidiMirrorStats final_stats;
    std::size_t expected_requests = 0U;

    for (std::size_t iteration = 0U; iteration < iterations + 32U; ++iteration) {
        zevryon::text::BidiMirrorStats stats;
        zevryon::text::BidiMirrorError error;
        const auto started = std::chrono::steady_clock::now();
        if (!zevryon::text::build_bidi_mirror_requests(
                codepoints,
                units,
                topology,
                visual,
                &output,
                &stats,
                &error)) {
            std::cerr << "mirroring benchmark failed: "
                      << zevryon::text::bidi_mirror_error_kind_name(error.kind)
                      << " at visual index " << error.visual_index << ' '
                      << error.message << '\n';
            return 1;
        }
        const auto ended = std::chrono::steady_clock::now();
        if (stats.exact_character_requests == 0U ||
            stats.best_fit_character_requests == 0U ||
            stats.glyph_only_requests == 0U ||
            stats.output_requests != output.requests.size()) {
            std::cerr << "mirroring benchmark rule coverage failed\n";
            return 1;
        }
        for (std::size_t index = 1U; index < output.requests.size(); ++index) {
            if (output.requests[index - 1U].visual_index >=
                output.requests[index].visual_index) {
                std::cerr << "mirror requests are not in visual order\n";
                return 1;
            }
        }
        if (expected_requests == 0U) {
            expected_requests = output.requests.size();
        } else if (expected_requests != output.requests.size()) {
            std::cerr << "mirror request count changed between iterations\n";
            return 1;
        }
        final_stats = stats;
        if (iteration >= 32U) {
            samples_ms.push_back(
                std::chrono::duration<double, std::milli>(ended - started).count());
        }
    }

    const auto resource = ledger.snapshot(
        zevryon::core::ResourceClass::BidiMirrorRequest);
    const std::size_t exact_output_bytes =
        output.requests.size() * sizeof(zevryon::text::BidiMirrorRequest);
    if (resource.current_bytes != exact_output_bytes ||
        resource.peak_bytes != exact_output_bytes) {
        std::cerr << "mirror request memory is not exact\n";
        return 1;
    }

    const double p50 = percentile(samples_ms, 50.0);
    const double p95 = percentile(samples_ms, 95.0);
    const double p99 = percentile(samples_ms, 99.0);
    const double maximum = *std::max_element(samples_ms.begin(), samples_ms.end());
    const double throughput_mib_per_second =
        (static_cast<double>(kFixtureBytes) / (1024.0 * 1024.0)) /
        (p50 / 1000.0);

    std::cout << '{'
              << "\"schema\":\"zevryon.bidi-mirroring-benchmark.v1\","
              << "\"uax_version\":\"Unicode 17.0.0 / UAX #9 revision 51\","
              << "\"fixture_bytes\":" << kFixtureBytes << ','
              << "\"iterations\":" << iterations << ','
              << "\"warmup_iterations\":32,"
              << "\"input_codepoints\":" << codepoints.size() << ','
              << "\"active_units\":" << final_stats.active_units << ','
              << "\"visual_units\":" << final_stats.visual_units << ','
              << "\"odd_level_units\":" << final_stats.odd_level_units << ','
              << "\"mirrored_property_hits\":"
              << final_stats.mirrored_property_hits << ','
              << "\"exact_character_requests\":"
              << final_stats.exact_character_requests << ','
              << "\"best_fit_character_requests\":"
              << final_stats.best_fit_character_requests << ','
              << "\"glyph_only_requests\":"
              << final_stats.glyph_only_requests << ','
              << "\"output_requests\":" << final_stats.output_requests << ','
              << "\"request_record_bytes\":"
              << sizeof(zevryon::text::BidiMirrorRequest) << ','
              << "\"p50_ms\":" << p50 << ','
              << "\"p95_ms\":" << p95 << ','
              << "\"p99_ms\":" << p99 << ','
              << "\"maximum_ms\":" << maximum << ','
              << "\"p50_mib_per_second\":" << throughput_mib_per_second << ','
              << "\"request_hard_limit_bytes\":" << resource.hard_limit_bytes << ','
              << "\"request_current_bytes\":" << resource.current_bytes << ','
              << "\"request_peak_bytes\":" << resource.peak_bytes << ','
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
