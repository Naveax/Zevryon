#include "coretext_discovery.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>

namespace {

using zevryon::text::CoreTextDiscoveryError;
using zevryon::text::CoreTextDiscoveryErrorKind;
using zevryon::text::CoreTextDiscoveryStats;
using zevryon::text::FontCatalogGeneration;

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

template <typename T>
bool spans_equal(std::span<const T> left, std::span<const T> right) {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin());
}

bool generations_equal(
    const FontCatalogGeneration& left,
    const FontCatalogGeneration& right) {
    const auto& left_catalog = left.catalog();
    const auto& right_catalog = right.catalog();
    return left.fingerprint() == right.fingerprint() &&
        spans_equal(left.discovery_records(), right.discovery_records()) &&
        spans_equal(left.families(), right.families()) &&
        spans_equal(
            std::span(left_catalog.faces.data(), left_catalog.faces.size()),
            std::span(right_catalog.faces.data(), right_catalog.faces.size())) &&
        spans_equal(
            std::span(
                left_catalog.coverage_ranges.data(),
                left_catalog.coverage_ranges.size()),
            std::span(
                right_catalog.coverage_ranges.data(),
                right_catalog.coverage_ranges.size())) &&
        spans_equal(
            std::span(
                left_catalog.script_buckets.data(),
                left_catalog.script_buckets.size()),
            std::span(
                right_catalog.script_buckets.data(),
                right_catalog.script_buckets.size())) &&
        spans_equal(
            std::span(
                left_catalog.script_face_ids.data(),
                left_catalog.script_face_ids.size()),
            std::span(
                right_catalog.script_face_ids.data(),
                right_catalog.script_face_ids.size()));
}

bool validate_generation(const FontCatalogGeneration& generation) {
    if (!require(generation.accounting_clean(), "generation accounting is not clean") ||
        !require(generation.within_hard_limits(), "generation exceeded hard limits") ||
        !require(!generation.discovery_records().empty(), "discovery records are empty") ||
        !require(!generation.families().empty(), "family records are empty") ||
        !require(
            generation.discovery_records().size() == generation.catalog().faces.size(),
            "discovery and catalog face counts differ")) {
        return false;
    }

    for (std::size_t index = 0U; index < generation.discovery_records().size(); ++index) {
        const auto& record = generation.discovery_records()[index];
        const auto& face = generation.catalog().faces[index];
        if (!require(
                record.face_id == static_cast<std::uint32_t>(index),
                "face id does not match canonical discovery order") ||
            !require(
                generation.identity(record.face_id).starts_with("coretext|"),
                "identity lacks the CoreText namespace") ||
            !require(
                !generation.family_name(record.family_index).empty(),
                "family name is empty") ||
            !require(
                (face.flags & zevryon::text::kFontFaceSystem) != 0U,
                "CoreText face lacks the system-font flag")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    constexpr std::size_t kDiscoveryHardLimit = 128U * 1024U * 1024U;
    constexpr std::size_t kCatalogHardLimit = 128U * 1024U * 1024U;

    std::shared_ptr<const FontCatalogGeneration> first;
    std::shared_ptr<const FontCatalogGeneration> second;
    CoreTextDiscoveryStats first_stats;
    CoreTextDiscoveryStats second_stats;
    CoreTextDiscoveryError first_error;
    CoreTextDiscoveryError second_error;

    if (!require(
            zevryon::text::build_coretext_generation(
                1U,
                kDiscoveryHardLimit,
                kCatalogHardLimit,
                &first,
                &first_stats,
                &first_error),
            "first CoreText enumeration failed: " + first_error.message) ||
        !require(first != nullptr, "first generation was not published") ||
        !validate_generation(*first) ||
        !require(
            zevryon::text::build_coretext_generation(
                2U,
                kDiscoveryHardLimit,
                kCatalogHardLimit,
                &second,
                &second_stats,
                &second_error),
            "second CoreText enumeration failed: " + second_error.message) ||
        !require(second != nullptr, "second generation was not published") ||
        !validate_generation(*second) ||
        !require(generations_equal(*first, *second), "enumerations are not deterministic") ||
        !require(first_stats.descriptors_seen > 0U, "no font descriptors were enumerated") ||
        !require(first_stats.faces_emitted > 0U, "no physical faces were emitted") ||
        !require(
            first_stats.faces_emitted == first->catalog().faces.size(),
            "emitted face count does not match catalog") ||
        !require(
            first_stats.descriptors_seen == second_stats.descriptors_seen &&
                first_stats.non_file_descriptors_skipped ==
                    second_stats.non_file_descriptors_skipped &&
                first_stats.faces_emitted == second_stats.faces_emitted &&
                first_stats.coverage_codepoints == second_stats.coverage_codepoints &&
                first_stats.coverage_ranges == second_stats.coverage_ranges &&
                first_stats.variable_faces == second_stats.variable_faces &&
                first_stats.color_faces == second_stats.color_faces &&
                first_stats.monospace_faces == second_stats.monospace_faces,
            "enumeration counters changed between unchanged runs")) {
        return 1;
    }

    std::shared_ptr<const FontCatalogGeneration> rejected = first;
    CoreTextDiscoveryStats rejected_stats;
    CoreTextDiscoveryError rejected_error;
    if (!require(
            !zevryon::text::build_coretext_generation(
                3U,
                1U,
                kCatalogHardLimit,
                &rejected,
                &rejected_stats,
                &rejected_error),
            "one-byte discovery budget unexpectedly succeeded") ||
        !require(rejected == nullptr, "budget failure published a stale generation") ||
        !require(
            rejected_error.kind == CoreTextDiscoveryErrorKind::SnapshotBuildFailed,
            "budget failure did not propagate through the snapshot boundary")) {
        return 1;
    }

    const auto fingerprint = first->fingerprint();
    const auto discovery_snapshot = first->discovery_resource_snapshot();
    const auto catalog_snapshot = first->catalog_resource_snapshot();
    std::cout << "descriptors=" << first_stats.descriptors_seen
              << " skipped_non_file=" << first_stats.non_file_descriptors_skipped
              << " faces=" << first_stats.faces_emitted
              << " coverage_ranges=" << first_stats.coverage_ranges
              << " coverage_codepoints=" << first_stats.coverage_codepoints
              << " duplicates=" << first_stats.duplicate_faces_skipped
              << " variable=" << first_stats.variable_faces
              << " color=" << first_stats.color_faces
              << " monospace=" << first_stats.monospace_faces
              << " discovery_current=" << discovery_snapshot.current_bytes
              << " discovery_peak=" << discovery_snapshot.peak_bytes
              << " catalog_current=" << catalog_snapshot.current_bytes
              << " catalog_peak=" << catalog_snapshot.peak_bytes
              << " fingerprint_high=" << fingerprint.high
              << " fingerprint_low=" << fingerprint.low
              << '\n';
    return 0;
}
