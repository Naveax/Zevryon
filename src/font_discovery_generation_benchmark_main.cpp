#include "font_discovery_generation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace zevryon::text;

constexpr std::size_t kFaceCount = 1024U;
constexpr std::size_t kFamilyCount = 128U;
constexpr std::size_t kWarmupIterations = 8U;
constexpr std::size_t kDiscoveryHardLimit = 108U * 1024U;
constexpr std::size_t kCatalogHardLimit = 64U * 1024U;

struct OwnedFace {
    std::string identity;
    std::string family;
    std::array<FontCoverageRange, 2> coverage{};
};

struct Fixture {
    std::vector<OwnedFace> storage;
    std::vector<FontDiscoveryFace> forward;
    std::vector<FontDiscoveryFace> reverse;
};

std::uint16_t flags_for_index(std::size_t index) noexcept {
    std::uint16_t flags = 0U;
    if (index % 5U == 0U) {
        flags = static_cast<std::uint16_t>(
            flags | static_cast<std::uint16_t>(kFontFaceVariable));
    }
    if (index % 7U == 0U) {
        flags = static_cast<std::uint16_t>(
            flags | static_cast<std::uint16_t>(kFontFaceColor));
    }
    return flags;
}

Fixture make_fixture() {
    constexpr std::array<ScriptId, 8> scripts{{
        ScriptId::Latn,
        ScriptId::Grek,
        ScriptId::Cyrl,
        ScriptId::Arab,
        ScriptId::Hebr,
        ScriptId::Deva,
        ScriptId::Hani,
        ScriptId::Zyyy,
    }};

    Fixture fixture;
    fixture.storage.reserve(kFaceCount);
    for (std::size_t index = 0U; index < kFaceCount; ++index) {
        const std::uint32_t range_base =
            0x1000U + static_cast<std::uint32_t>(index % 4096U) * 0x10U;
        const std::string identity =
            "adapter/font-" + std::to_string(100000ULL + index) + "#0";
        const std::string family =
            "Family-" + std::to_string(1000ULL + (index % kFamilyCount));
        fixture.storage.push_back(OwnedFace{
            identity,
            family,
            {{{0x0020U, 0x007eU}, {range_base, range_base + 7U}}},
        });
    }

    fixture.forward.reserve(kFaceCount);
    for (std::size_t index = 0U; index < fixture.storage.size(); ++index) {
        const OwnedFace& owned = fixture.storage[index];
        fixture.forward.push_back(FontDiscoveryFace{
            owned.identity,
            owned.family,
            static_cast<std::uint16_t>(100U + (index % 10U) * 100U),
            static_cast<std::uint8_t>(1U + (index % 9U)),
            static_cast<FontSlant>(index % 3U),
            scripts[index % scripts.size()],
            flags_for_index(index),
            owned.coverage,
        });
    }
    fixture.reverse = fixture.forward;
    std::reverse(fixture.reverse.begin(), fixture.reverse.end());
    return fixture;
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
        : 64U;
    if (iterations == 0U) {
        std::cerr << "iterations must be positive\n";
        return 2;
    }

    const Fixture fixture = make_fixture();
    std::vector<double> durations_ms;
    durations_ms.reserve(iterations);
    FontGenerationFingerprint expected_fingerprint{};
    bool have_fingerprint = false;
    std::shared_ptr<const FontCatalogGeneration> first_generation;
    std::shared_ptr<const FontCatalogGeneration> last_generation;
    FontDiscoveryStats last_stats;

    for (std::size_t iteration = 0U;
         iteration < kWarmupIterations + iterations;
         ++iteration) {
        const std::span<const FontDiscoveryFace> input =
            iteration % 2U == 0U
                ? std::span<const FontDiscoveryFace>(fixture.forward)
                : std::span<const FontDiscoveryFace>(fixture.reverse);
        std::shared_ptr<const FontCatalogGeneration> generation;
        FontDiscoveryStats stats;
        FontDiscoveryError error;
        const auto begin = std::chrono::steady_clock::now();
        const bool built = build_font_catalog_generation(
            static_cast<std::uint64_t>(iteration) + 1U,
            input,
            kDiscoveryHardLimit,
            kCatalogHardLimit,
            &generation,
            &stats,
            &error);
        const auto end = std::chrono::steady_clock::now();
        if (!built) {
            std::cerr << "generation build failed: " << error.message << '\n';
            return 3;
        }
        if (!have_fingerprint) {
            expected_fingerprint = generation->fingerprint();
            have_fingerprint = true;
            first_generation = generation;
        } else if (generation->fingerprint() != expected_fingerprint) {
            std::cerr << "enumeration order changed generation fingerprint\n";
            return 4;
        }
        if (iteration >= kWarmupIterations) {
            durations_ms.push_back(
                std::chrono::duration<double, std::milli>(end - begin).count());
        }
        last_stats = stats;
        last_generation = std::move(generation);
    }

    FontCatalogGenerationStore store;
    if (store.publish(first_generation) != FontGenerationPublishResult::Published ||
        store.publish(last_generation) !=
            FontGenerationPublishResult::IdenticalSnapshot) {
        std::cerr << "generation publication suppression contract failed\n";
        return 5;
    }

    std::sort(durations_ms.begin(), durations_ms.end());
    const double p50 = percentile(durations_ms, 0.50);
    const double p95 = percentile(durations_ms, 0.95);
    const double p99 = percentile(durations_ms, 0.99);
    const auto discovery = last_generation->discovery_resource_snapshot();
    const auto catalog = last_generation->catalog_resource_snapshot();

    std::cout << std::fixed << std::setprecision(6)
              << "{\"schema\":\"zevryon.font-discovery-generation-benchmark.v1\"," 
              << "\"faces\":" << kFaceCount << ','
              << "\"families\":" << kFamilyCount << ','
              << "\"iterations\":" << iterations << ','
              << "\"warmup_iterations\":" << kWarmupIterations << ','
              << "\"identity_bytes\":" << last_stats.identity_bytes << ','
              << "\"family_bytes\":" << last_stats.family_bytes << ','
              << "\"snapshot_bytes\":" << last_stats.snapshot_bytes << ','
              << "\"catalog_faces\":" << last_generation->catalog().faces.size() << ','
              << "\"catalog_ranges\":" << last_generation->catalog().coverage_ranges.size() << ','
              << "\"fingerprint_high\":" << expected_fingerprint.high << ','
              << "\"fingerprint_low\":" << expected_fingerprint.low << ','
              << "\"p50_ms\":" << p50 << ','
              << "\"p95_ms\":" << p95 << ','
              << "\"p99_ms\":" << p99 << ','
              << "\"maximum_ms\":" << durations_ms.back() << ','
              << "\"discovery_hard_limit_bytes\":" << kDiscoveryHardLimit << ','
              << "\"discovery_current_bytes\":" << discovery.current_bytes << ','
              << "\"discovery_peak_bytes\":" << discovery.peak_bytes << ','
              << "\"catalog_hard_limit_bytes\":" << kCatalogHardLimit << ','
              << "\"catalog_current_bytes\":" << catalog.current_bytes << ','
              << "\"catalog_peak_bytes\":" << catalog.peak_bytes << ','
              << "\"discovery_rejections\":" << discovery.rejected_reservations << ','
              << "\"catalog_rejections\":" << catalog.rejected_reservations << ','
              << "\"accounting_clean\":"
              << (last_generation->accounting_clean() ? "true" : "false") << ','
              << "\"within_hard_limits\":"
              << (last_generation->within_hard_limits() ? "true" : "false")
              << "}\n";
    return 0;
}
