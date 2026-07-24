#include "directwrite_discovery.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>

namespace {

using zevryon::text::DirectWriteDiscoveryError;
using zevryon::text::DirectWriteDiscoveryStats;
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
        if (!require(
                record.face_id == static_cast<std::uint32_t>(index),
                "face id does not match canonical discovery order") ||
            !require(
                generation.identity(record.face_id).starts_with("directwrite|"),
                "identity lacks the DirectWrite namespace") ||
            !require(
                !generation.family_name(record.family_index).empty(),
                "family name is empty")) {
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
    DirectWriteDiscoveryStats first_stats;
    DirectWriteDiscoveryStats second_stats;
    DirectWriteDiscoveryError first_error;
    DirectWriteDiscoveryError second_error;

    if (!require(
            zevryon::text::build_directwrite_generation(
                1U,
                kDiscoveryHardLimit,
                kCatalogHardLimit,
                &first,
                &first_stats,
                &first_error),
            "first DirectWrite enumeration failed: " + first_error.message) ||
        !require(first != nullptr, "first generation was not published") ||
        !validate_generation(*first) ||
        !require(
            zevryon::text::build_directwrite_generation(
                2U,
                kDiscoveryHardLimit,
                kCatalogHardLimit,
                &second,
                &second_stats,
                &second_error),
            "second DirectWrite enumeration failed: " + second_error.message) ||
        !require(second != nullptr, "second generation was not published") ||
        !validate_generation(*second) ||
        !require(generations_equal(*first, *second), "enumerations are not deterministic") ||
        !require(first_stats.families_seen > 0U, "no font families were enumerated") ||
        !require(first_stats.fonts_seen > 0U, "no fonts were enumerated") ||
        !require(
            first_stats.faces_emitted == first->catalog().faces.size(),
            "emitted face count does not match catalog") ||
        !require(
            first_stats.families_seen == second_stats.families_seen &&
                first_stats.fonts_seen == second_stats.fonts_seen &&
                first_stats.faces_emitted == second_stats.faces_emitted &&
                first_stats.font_files_seen == second_stats.font_files_seen &&
                first_stats.coverage_codepoints == second_stats.coverage_codepoints &&
                first_stats.coverage_ranges == second_stats.coverage_ranges,
            "enumeration counters changed between unchanged runs")) {
        return 1;
    }

    std::cout << "families=" << first_stats.families_seen
              << " fonts=" << first_stats.fonts_seen
              << " faces=" << first_stats.faces_emitted
              << " files=" << first_stats.font_files_seen
              << " coverage_ranges=" << first_stats.coverage_ranges
              << " coverage_codepoints=" << first_stats.coverage_codepoints
              << " duplicates=" << first_stats.duplicate_faces_skipped
              << " simulations=" << first_stats.simulated_faces
              << " monospace=" << first_stats.monospace_faces
              << '\n';
    return 0;
}
