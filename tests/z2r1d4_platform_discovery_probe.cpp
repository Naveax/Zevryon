#if !defined(_WIN32)
#error "Z2R-1D4 platform discovery probe supports Windows only"
#endif

#include "directwrite_discovery.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using namespace zevryon::text;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kDiscoveryLimit = 256U * 1024U * 1024U;
constexpr std::size_t kCatalogLimit = 256U * 1024U * 1024U;
constexpr std::size_t kWarmupIterations = 1U;
constexpr std::size_t kMeasuredIterations = 5U;

struct SemanticSnapshot {
    std::uint64_t fingerprint_high{0U};
    std::uint64_t fingerprint_low{0U};
    std::uint64_t source_groups_seen{0U};
    std::uint64_t source_records_seen{0U};
    std::uint64_t source_records_skipped{0U};
    std::uint64_t faces_emitted{0U};
    std::uint64_t duplicate_faces_skipped{0U};
    std::uint64_t coverage_codepoints{0U};
    std::uint64_t coverage_ranges{0U};
    std::uint64_t bitmap_planes{0U};
    std::uint64_t variable_faces{0U};
    std::uint64_t color_faces{0U};
    std::uint64_t monospace_faces{0U};
    std::size_t discovery_records{0U};
    std::size_t family_records{0U};
    std::size_t catalog_faces{0U};
    std::size_t discovery_current_bytes{0U};
    std::size_t discovery_peak_bytes{0U};
    std::size_t catalog_current_bytes{0U};
    std::size_t catalog_peak_bytes{0U};

    bool operator==(const SemanticSnapshot&) const = default;
};

struct BuildResult {
    std::shared_ptr<const FontCatalogGeneration> generation;
    SemanticSnapshot semantic;
};

const char* platform_name() noexcept {
    return "windows";
}

const char* adapter_name() noexcept {
    return "directwrite";
}

bool build_generation(std::uint64_t generation_id, BuildResult* output) {
    DirectWriteDiscoveryStats stats;
    DirectWriteDiscoveryError error;
    if (!build_directwrite_generation(
            generation_id,
            kDiscoveryLimit,
            kCatalogLimit,
            &output->generation,
            &stats,
            &error)) {
        std::cerr << "DirectWrite discovery failed: "
                  << directwrite_discovery_error_kind_name(error.kind)
                  << " native=" << error.native_error
                  << " message=" << error.message << '\n';
        return false;
    }
    output->semantic.source_groups_seen = stats.families_seen;
    output->semantic.source_records_seen = stats.fonts_seen;
    output->semantic.source_records_skipped = stats.simulated_fonts_skipped;
    output->semantic.faces_emitted = stats.faces_emitted;
    output->semantic.duplicate_faces_skipped = stats.duplicate_faces_skipped;
    output->semantic.coverage_codepoints = stats.coverage_codepoints;
    output->semantic.coverage_ranges = stats.coverage_ranges;
    output->semantic.variable_faces = stats.variable_faces;
    output->semantic.color_faces = stats.color_faces;
    output->semantic.monospace_faces = stats.monospace_faces;

    if (output->generation == nullptr ||
        output->generation->discovery_records().empty() ||
        !output->generation->accounting_clean() ||
        !output->generation->within_hard_limits()) {
        std::cerr << "platform discovery generation failed postconditions\n";
        return false;
    }

    const auto discovery = output->generation->discovery_resource_snapshot();
    const auto catalog = output->generation->catalog_resource_snapshot();
    const auto fingerprint = output->generation->fingerprint();
    output->semantic.fingerprint_high = fingerprint.high;
    output->semantic.fingerprint_low = fingerprint.low;
    output->semantic.discovery_records = output->generation->discovery_records().size();
    output->semantic.family_records = output->generation->families().size();
    output->semantic.catalog_faces = output->generation->catalog().faces.size();
    output->semantic.discovery_current_bytes = discovery.current_bytes;
    output->semantic.discovery_peak_bytes = discovery.peak_bytes;
    output->semantic.catalog_current_bytes = catalog.current_bytes;
    output->semantic.catalog_peak_bytes = catalog.peak_bytes;
    return output->semantic.faces_emitted ==
               static_cast<std::uint64_t>(output->semantic.discovery_records) &&
           output->semantic.discovery_records == output->semantic.catalog_faces &&
           output->semantic.discovery_current_bytes <= output->semantic.discovery_peak_bytes &&
           output->semantic.catalog_current_bytes <= output->semantic.catalog_peak_bytes;
}

double percentile(const std::vector<double>& sorted, double fraction) {
    const double position = fraction * static_cast<double>(sorted.size() - 1U);
    const std::size_t lower = static_cast<std::size_t>(position);
    const std::size_t upper = std::min(lower + 1U, sorted.size() - 1U);
    const double weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

void emit_semantic(const SemanticSnapshot& value) {
    std::cout
        << "\"fingerprint_high\":" << value.fingerprint_high << ','
        << "\"fingerprint_low\":" << value.fingerprint_low << ','
        << "\"source_groups_seen\":" << value.source_groups_seen << ','
        << "\"source_records_seen\":" << value.source_records_seen << ','
        << "\"source_records_skipped\":" << value.source_records_skipped << ','
        << "\"faces_emitted\":" << value.faces_emitted << ','
        << "\"duplicate_faces_skipped\":" << value.duplicate_faces_skipped << ','
        << "\"coverage_codepoints\":" << value.coverage_codepoints << ','
        << "\"coverage_ranges\":" << value.coverage_ranges << ','
        << "\"bitmap_planes\":" << value.bitmap_planes << ','
        << "\"variable_faces\":" << value.variable_faces << ','
        << "\"color_faces\":" << value.color_faces << ','
        << "\"monospace_faces\":" << value.monospace_faces << ','
        << "\"discovery_records\":" << value.discovery_records << ','
        << "\"family_records\":" << value.family_records << ','
        << "\"catalog_faces\":" << value.catalog_faces << ','
        << "\"discovery_current_bytes\":" << value.discovery_current_bytes << ','
        << "\"discovery_peak_bytes\":" << value.discovery_peak_bytes << ','
        << "\"catalog_current_bytes\":" << value.catalog_current_bytes << ','
        << "\"catalog_peak_bytes\":" << value.catalog_peak_bytes;
}

} // namespace

int main() {
    std::vector<double> durations;
    durations.reserve(kMeasuredIterations);
    SemanticSnapshot expected;
    bool have_expected = false;

    for (std::size_t iteration = 0U;
         iteration < kWarmupIterations + kMeasuredIterations;
         ++iteration) {
        BuildResult result;
        const auto begin = Clock::now();
        if (!build_generation(static_cast<std::uint64_t>(iteration + 1U), &result)) {
            return 1;
        }
        const auto end = Clock::now();
        if (!have_expected) {
            expected = result.semantic;
            have_expected = true;
        } else if (!(result.semantic == expected)) {
            std::cerr << "platform discovery semantic state changed within one run\n";
            return 1;
        }
        if (iteration >= kWarmupIterations) {
            durations.push_back(
                std::chrono::duration<double, std::milli>(end - begin).count());
        }
    }

    if (!have_expected || durations.size() != kMeasuredIterations) {
        return 1;
    }
    std::sort(durations.begin(), durations.end());
    std::cout << std::fixed << std::setprecision(6)
              << "{\"schema\":\"zevryon.platform-font-discovery-benchmark.v1\","
              << "\"platform\":\"" << platform_name() << "\","
              << "\"adapter\":\"" << adapter_name() << "\","
              << "\"warmup_iterations\":" << kWarmupIterations << ','
              << "\"iterations\":" << kMeasuredIterations << ',';
    emit_semantic(expected);
    std::cout << ','
              << "\"p50_ms\":" << percentile(durations, 0.50) << ','
              << "\"p95_ms\":" << percentile(durations, 0.95) << ','
              << "\"p99_ms\":" << percentile(durations, 0.99) << ','
              << "\"maximum_ms\":" << durations.back() << ','
              << "\"accounting_clean\":true,"
              << "\"within_hard_limits\":true,"
              << "\"passed\":true}\n";
    return 0;
}
