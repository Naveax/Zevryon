#include "coretext_discovery.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>

namespace {

using namespace zevryon::text;

constexpr std::size_t kDiscoveryLimit = 128U * 1024U * 1024U;
constexpr std::size_t kCatalogLimit = 128U * 1024U * 1024U;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

template <typename Left, typename Right>
bool equal_ranges(const Left& left, const Right& right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin());
}

bool build(
    std::uint64_t generation_id,
    std::shared_ptr<const FontCatalogGeneration>* output,
    CoreTextDiscoveryStats* stats) {
    CoreTextDiscoveryError error;
    if (!build_coretext_generation(
            generation_id,
            kDiscoveryLimit,
            kCatalogLimit,
            output,
            stats,
            &error)) {
        std::cerr << "CoreText discovery failed: "
                  << coretext_discovery_error_kind_name(error.kind)
                  << " generation="
                  << font_discovery_error_kind_name(error.generation_error)
                  << " catalog="
                  << font_catalog_error_kind_name(error.catalog_error)
                  << " descriptor=" << error.descriptor_index
                  << " message=" << error.message << '\n';
        return false;
    }
    return true;
}

bool validate_generation(
    const FontCatalogGeneration& generation,
    const CoreTextDiscoveryStats& stats) {
    bool ok = true;
    const auto records = generation.discovery_records();
    const auto& catalog = generation.catalog();
    ok &= expect(!records.empty(), "system CoreText enumeration must not be empty");
    ok &= expect(
        records.size() == catalog.faces.size(),
        "discovery records and catalog faces must align");
    ok &= expect(
        stats.faces_emitted == static_cast<std::uint64_t>(records.size()),
        "adapter face count must match generation records");
    ok &= expect(
        stats.descriptors_seen == stats.descriptors_skipped +
                                      stats.faces_emitted +
                                      stats.duplicate_faces_skipped,
        "every descriptor must be skipped, emitted, or deduplicated exactly once");
    ok &= expect(stats.coverage_codepoints > 0U, "coverage bitmap must contain codepoints");
    ok &= expect(stats.coverage_ranges > 0U, "coverage bitmap must emit ranges");
    ok &= expect(stats.bitmap_planes > 0U, "coverage bitmap must expose planes");
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

        const FontFaceRecord& face = catalog.faces[index];
        ok &= expect(
            face.stable_key == static_cast<std::uint64_t>(index) + 1U,
            "stable keys must follow canonical order");
        ok &= expect(face.coverage_count != 0U, "every emitted face must have coverage");
        ok &= expect(
            (face.flags & kFontFaceSystem) != 0U,
            "every CoreText collection face must carry the system flag");
        const std::size_t first = face.coverage_offset;
        const std::size_t count = face.coverage_count;
        ok &= expect(
            first <= catalog.coverage_ranges.size() &&
                count <= catalog.coverage_ranges.size() - first,
            "face coverage slice must remain bounded");
        if (first <= catalog.coverage_ranges.size() &&
            count <= catalog.coverage_ranges.size() - first) {
            for (std::size_t offset = 0U; offset < count; ++offset) {
                const FontCoverageRange range = catalog.coverage_ranges[first + offset];
                ok &= expect(
                    range.first <= range.last && range.last <= 0x10ffffU,
                    "CoreText coverage ranges must contain valid scalar bounds");
                ok &= expect(
                    range.last < 0xd800U || range.first > 0xdfffU,
                    "CoreText coverage must exclude surrogate code points");
                if (offset != 0U) {
                    const FontCoverageRange prior =
                        catalog.coverage_ranges[first + offset - 1U];
                    ok &= expect(
                        static_cast<std::uint64_t>(prior.last) + 1U < range.first,
                        "canonical coverage ranges must be sorted and non-adjacent");
                }
            }
        }
    }
    return ok;
}

bool test_budget_failures() {
    bool ok = true;
    std::shared_ptr<const FontCatalogGeneration> output;
    CoreTextDiscoveryStats stats;
    CoreTextDiscoveryError error;

    ok &= expect(
        !build_coretext_generation(
            100U,
            1U,
            kCatalogLimit,
            &output,
            &stats,
            &error),
        "tiny discovery budget must fail");
    ok &= expect(output == nullptr, "discovery budget failure must publish no generation");
    ok &= expect(
        error.kind == CoreTextDiscoveryErrorKind::SnapshotBuildFailed &&
            error.generation_error == FontDiscoveryErrorKind::SnapshotBudgetExceeded,
        "discovery budget failure must preserve nested error kind");

    error = {};
    stats = {};
    ok &= expect(
        !build_coretext_generation(
            101U,
            kDiscoveryLimit,
            1U,
            &output,
            &stats,
            &error),
        "tiny catalog budget must fail");
    ok &= expect(output == nullptr, "catalog budget failure must publish no generation");
    ok &= expect(
        error.kind == CoreTextDiscoveryErrorKind::SnapshotBuildFailed &&
            error.generation_error == FontDiscoveryErrorKind::CatalogBuildFailed &&
            error.catalog_error == FontCatalogErrorKind::OutputBudgetExceeded,
        "catalog budget failure must preserve nested error kinds");
    return ok;
}

} // namespace

int main() {
    std::shared_ptr<const FontCatalogGeneration> first;
    std::shared_ptr<const FontCatalogGeneration> second;
    CoreTextDiscoveryStats first_stats;
    CoreTextDiscoveryStats second_stats;

    if (!build(1U, &first, &first_stats) ||
        !build(2U, &second, &second_stats)) {
        return 1;
    }

    bool ok = true;
    ok &= validate_generation(*first, first_stats);
    ok &= validate_generation(*second, second_stats);
    ok &= expect(
        first->fingerprint() == second->fingerprint(),
        "unchanged CoreText state must reproduce the same fingerprint");
    ok &= expect(
        equal_ranges(first->discovery_records(), second->discovery_records()),
        "unchanged CoreText state must reproduce discovery records");
    ok &= expect(
        equal_ranges(first->families(), second->families()),
        "unchanged CoreText state must reproduce family records");
    ok &= expect(
        first->catalog().faces == second->catalog().faces,
        "unchanged CoreText state must reproduce catalog faces");
    ok &= expect(
        first->catalog().coverage_ranges == second->catalog().coverage_ranges,
        "unchanged CoreText state must reproduce coverage");
    ok &= expect(
        first->catalog().script_buckets == second->catalog().script_buckets,
        "unchanged CoreText state must reproduce script buckets");
    ok &= expect(
        first->catalog().script_face_ids == second->catalog().script_face_ids,
        "unchanged CoreText state must reproduce script face IDs");
    ok &= test_budget_failures();

    if (!ok) {
        return 1;
    }

    const auto discovery = first->discovery_resource_snapshot();
    const auto catalog = first->catalog_resource_snapshot();
    std::cout << "CoreText discovery tests passed: descriptors="
              << first_stats.descriptors_seen
              << " skipped=" << first_stats.descriptors_skipped
              << " faces=" << first_stats.faces_emitted
              << " ranges=" << first_stats.coverage_ranges
              << " codepoints=" << first_stats.coverage_codepoints
              << " planes=" << first_stats.bitmap_planes
              << " duplicates=" << first_stats.duplicate_faces_skipped
              << " variable=" << first_stats.variable_faces
              << " color=" << first_stats.color_faces
              << " monospace=" << first_stats.monospace_faces
              << " discovery_current=" << discovery.current_bytes
              << " discovery_peak=" << discovery.peak_bytes
              << " catalog_current=" << catalog.current_bytes
              << " catalog_peak=" << catalog.peak_bytes
              << " fingerprint_high=" << first->fingerprint().high
              << " fingerprint_low=" << first->fingerprint().low
              << '\n';
    return 0;
}
