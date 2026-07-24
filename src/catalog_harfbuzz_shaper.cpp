#include "catalog_harfbuzz_shaper.hpp"

#include <memory>
#include <utility>

namespace zevryon::text {
namespace {

void clear_error(CatalogHarfBuzzShapingError* error) noexcept {
    if (error != nullptr) {
        error->kind = CatalogHarfBuzzShapingErrorKind::None;
        error->resource_error = {};
        error->shaping_error = {};
        error->message.clear();
    }
}

bool fail(
    CatalogHarfBuzzShapingErrorKind kind,
    const char* message,
    CatalogHarfBuzzShapingError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

void set_resource_failure_message(
    CatalogHarfBuzzShapingError* error) noexcept {
    try {
        error->message = "catalog font resource resolution failed: ";
        error->message += catalog_font_resource_error_kind_name(
            error->resource_error.kind);
    } catch (...) {
        error->message.clear();
    }
}

void set_shaping_failure_message(
    CatalogHarfBuzzShapingError* error) noexcept {
    try {
        error->message = "catalog-backed HarfBuzz shaping failed: ";
        error->message += harfbuzz_shaping_error_kind_name(
            error->shaping_error.kind);
    } catch (...) {
        error->message.clear();
    }
}

} // namespace

const char* catalog_harfbuzz_shaping_error_kind_name(
    CatalogHarfBuzzShapingErrorKind kind) noexcept {
    switch (kind) {
    case CatalogHarfBuzzShapingErrorKind::None:
        return "none";
    case CatalogHarfBuzzShapingErrorKind::InvalidArgument:
        return "invalid_argument";
    case CatalogHarfBuzzShapingErrorKind::ResourceResolutionFailed:
        return "resource_resolution_failed";
    case CatalogHarfBuzzShapingErrorKind::ShapingFailed:
        return "shaping_failed";
    }
    return "unknown";
}

bool shape_catalog_harfbuzz_segment(
    const CatalogHarfBuzzShapingRequest& request,
    ShapedGlyphRun* output,
    CatalogHarfBuzzShapingStats* stats,
    CatalogHarfBuzzShapingError* error) noexcept {
    if (output != nullptr) {
        output->release();
    }
    if (stats != nullptr) {
        *stats = {};
    }
    clear_error(error);

    if (output == nullptr || stats == nullptr || error == nullptr ||
        !request.generation || request.cache == nullptr ||
        request.staging_hard_limit == 0U) {
        return fail(
            CatalogHarfBuzzShapingErrorKind::InvalidArgument,
            "generation, cache, positive staging limit, output, stats, and error are required",
            error);
    }

    const std::shared_ptr<const FontCatalogGeneration> generation =
        request.generation;
    std::shared_ptr<const VerifiedFontResource> resource;
    CatalogFontResourceStats resource_stats;
    CatalogFontResourceError resource_error;
    if (!resolve_catalog_font_resource(
            generation,
            request.face_id,
            request.staging_hard_limit,
            request.cache,
            &resource,
            &resource_stats,
            &resource_error)) {
        stats->resource = resource_stats;
        error->kind =
            CatalogHarfBuzzShapingErrorKind::ResourceResolutionFailed;
        error->resource_error = std::move(resource_error);
        set_resource_failure_message(error);
        return false;
    }

    stats->resource = resource_stats;
    stats->resource_resolved = true;

    HarfBuzzShapingRequest shaping_request;
    shaping_request.face_index = resource->view().face_index();
    shaping_request.codepoints = request.codepoints;
    shaping_request.grapheme_boundaries = request.grapheme_boundaries;
    shaping_request.first_cluster = request.first_cluster;
    shaping_request.cluster_limit = request.cluster_limit;
    shaping_request.script = request.script;
    shaping_request.direction = request.direction;
    shaping_request.language = request.language;
    shaping_request.features = request.features;
    shaping_request.variations = request.variations;
    shaping_request.x_scale = request.x_scale;
    shaping_request.y_scale = request.y_scale;
    shaping_request.beginning_of_text = request.beginning_of_text;
    shaping_request.end_of_text = request.end_of_text;
    shaping_request.produce_unsafe_to_concat =
        request.produce_unsafe_to_concat;
    shaping_request.verified_font_resource = std::move(resource);

    HarfBuzzShapingStats shaping_stats;
    HarfBuzzShapingError shaping_error;
    if (!shape_harfbuzz_segment(
            shaping_request,
            output,
            &shaping_stats,
            &shaping_error)) {
        stats->shaping = shaping_stats;
        error->kind = CatalogHarfBuzzShapingErrorKind::ShapingFailed;
        error->shaping_error = std::move(shaping_error);
        set_shaping_failure_message(error);
        return false;
    }

    stats->shaping = shaping_stats;
    stats->shaping_completed = true;
    return true;
}

} // namespace zevryon::text
