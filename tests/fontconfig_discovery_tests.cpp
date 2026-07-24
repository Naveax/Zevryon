#include "fontconfig_discovery.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>

namespace {

using namespace zevryon::text;

constexpr std::size_t kDiscoveryLimit = 32U * 1024U * 1024U;
constexpr std::size_t kCatalogLimit = 32U * 1024U * 1024U;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

bool build(
    std::uint64_t generation_id,
    std::shared_ptr<const FontCatalogGeneration>* output,
    FontconfigDiscoveryStats* stats) {
    FontconfigDiscoveryError error;
    if (!build_fontconfig_generation(
            generation_id,
            kDiscoveryLimit,
            kCatalogLimit,
            output,
            stats,
            &error)) {
        std::cerr << "fontconfig discovery failed: "
                  << fontconfig_discovery_error_kind_name(error.kind)
                  << " generation="
                  << font_discovery_error_kind_name(error.generation_error)
                  << " catalog="
                  << font_catalog_error_kind_name(error.catalog_error)
                  << " pattern=" << error.pattern_index
                  << " message=" << error.message << '\n';
        return false;
    }
    return true;
}

bool validate_generation(
    const FontCatalogGeneration& generation,
    const FontconfigDiscoveryStats& stats) {
    bool ok = true;
    const auto records = generation.discovery_records();
    ok &= expect(!records.empty(), "system Fontconfig enumeration must not be empty");
    ok &= expect(
        records.size() == generation.catalog().faces.size(),
        "discovery records and catalog faces must align");
    ok &= expect(
        stats.faces_emitted == records.size(),
        "adapter face count must match generation records");
    ok &= expect(
        stats.patterns_seen >= stats.faces_emitted,
        "deduplication cannot increase face count");
    ok &= expect(stats.charset_codepoints > 0U, "coverage scan must see codepoints");
    ok &= expect(stats.coverage_ranges > 0U, "coverage scan must emit ranges");
    ok &= expect(generation.accounting_clean(), "generation accounting must remain clean");
    ok &= expect(generation.within_hard_limits(), "generation must remain within limits");

    std::string_view previous;
    for (std::size_t index = 0U; index < records.size(); ++index) {
        const std::string_view identity = generation.identity(
            static_cast<FontFaceId>(index));
        ok &= expect(!identity.empty(), "every face must retain a native identity");
        if (index != 0U) {
            ok &= expect(previous < identity, "canonical identities must be strictly sorted");
        }
        previous = identity;

        const FontFaceRecord& face = generation.catalog().faces[index];
        ok &= expect(face.stable_key == index + 1U, "stable keys must follow canonical order");
        ok &= expect(face.coverage_count != 0U, "every emitted face must have coverage");
    }
    return ok;
}

} // namespace

int main() {
    std::shared_ptr<const FontCatalogGeneration> first;
    std::shared_ptr<const FontCatalogGeneration> second;
    FontconfigDiscoveryStats first_stats;
    FontconfigDiscoveryStats second_stats;

    if (!build(1U, &first, &first_stats) ||
        !build(2U, &second, &second_stats)) {
        return 1;
    }

    bool ok = true;
    ok &= validate_generation(*first, first_stats);
    ok &= validate_generation(*second, second_stats);
    ok &= expect(
        first->fingerprint() == second->fingerprint(),
        "unchanged Fontconfig state must reproduce the same fingerprint");
    ok &= expect(
        first->discovery_records() == second->discovery_records(),
        "unchanged Fontconfig state must reproduce discovery records");
    ok &= expect(
        first->catalog().faces == second->catalog().faces,
        "unchanged Fontconfig state must reproduce catalog faces");
    ok &= expect(
        first->catalog().coverage_ranges == second->catalog().coverage_ranges,
        "unchanged Fontconfig state must reproduce coverage");

    if (!ok) {
        return 1;
    }
    std::cout << "Fontconfig discovery tests passed: faces="
              << first_stats.faces_emitted
              << " ranges=" << first_stats.coverage_ranges
              << " codepoints=" << first_stats.charset_codepoints
              << " duplicates=" << first_stats.duplicate_patterns_skipped
              << '\n';
    return 0;
}
