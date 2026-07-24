#include "cached_catalog_harfbuzz_shaper.hpp"

#include <memory>
#include <utility>

namespace zevryon::text {
namespace {

void clear_error(CachedCatalogHarfBuzzShapingError* error) noexcept {
    if (error != nullptr) {
        error->kind = CachedCatalogHarfBuzzShapingErrorKind::None;
        error->face_cache_error = {};
        error->shaping_error = {};
        error->message.clear();
    }
}

bool fail(
    CachedCatalogHarfBuzzShapingErrorKind kind,
    const char* message,
    CachedCatalogHarfBuzzShapingError* error) noexcept {
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

void set_cache_failure_message(
    CachedCatalogHarfBuzzShapingError* error) noexcept {
    try {
        error->message = "prepared HarfBuzz face cache acquisition failed: ";
        error->message += prepared_harfbuzz_face_cache_error_kind_name(
            error->face_cache_error.kind);
    } catch (...) {
        error->message.clear();
    }
}

void set_shaping_failure_message(
    CachedCatalogHarfBuzzShapingError* error) noexcept {
    try {
        error->message = "cache-backed catalog HarfBuzz shaping failed: ";
        error->message += bound_catalog_harfbuzz_shaping_error_kind_name(
            error->shaping_error.kind);
    } catch (...) {
        error->message.clear();
    }
}

} // namespace

const char* cached_catalog_harfbuzz_shaping_error_kind_name(
    CachedCatalogHarfBuzzShapingErrorKind kind) noexcept {
    switch (kind) {
    case CachedCatalogHarfBuzzShapingErrorKind::None:
        return "none";
    case CachedCatalogHarfBuzzShapingErrorKind::InvalidArgument:
        return "invalid_argument";
    case CachedCatalogHarfBuzzShapingErrorKind::FaceCacheFailed:
        return "face_cache_failed";
    case CachedCatalogHarfBuzzShapingErrorKind::ShapingFailed:
        return "shaping_failed";
    }
    return "unknown";
}

bool shape_cached_catalog_harfbuzz_segment(
    const CachedCatalogHarfBuzzShapingRequest& request,
    ShapedGlyphRun* output,
    CachedCatalogHarfBuzzShapingStats* stats,
    CachedCatalogHarfBuzzShapingError* error) noexcept {
    if (output != nullptr) {
        output->release();
    }
    if (stats != nullptr) {
        *stats = {};
    }
    clear_error(error);

    if (output == nullptr || stats == nullptr || error == nullptr ||
        request.binding == nullptr || request.prepared_face_cache == nullptr) {
        return fail(
            CachedCatalogHarfBuzzShapingErrorKind::InvalidArgument,
            "binding, prepared-face cache, output, stats, and error are required",
            error);
    }

    const CatalogFontFaceBinding binding = *request.binding;
    if (!binding.valid()) {
        return fail(
            CachedCatalogHarfBuzzShapingErrorKind::InvalidArgument,
            "catalog font-face binding is invalid",
            error);
    }

    std::shared_ptr<const PreparedHarfBuzzFace> prepared;
    PreparedHarfBuzzFaceCacheStats cache_stats;
    PreparedHarfBuzzFaceCacheError cache_error;
    if (!request.prepared_face_cache->get_or_prepare(
            binding,
            &prepared,
            &cache_stats,
            &cache_error)) {
        stats->face_cache = cache_stats;
        error->kind = CachedCatalogHarfBuzzShapingErrorKind::FaceCacheFailed;
        error->face_cache_error = std::move(cache_error);
        set_cache_failure_message(error);
        return false;
    }

    stats->face_cache = cache_stats;
    stats->face_acquired = true;

    BoundCatalogHarfBuzzShapingRequest shaping_request;
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
    shaping_request.prepared_harfbuzz_face = std::move(prepared);

    BoundCatalogHarfBuzzShapingStats shaping_stats;
    BoundCatalogHarfBuzzShapingError shaping_error;
    if (!shape_bound_catalog_harfbuzz_segment(
            shaping_request,
            output,
            &shaping_stats,
            &shaping_error)) {
        stats->shaping = shaping_stats;
        error->kind = CachedCatalogHarfBuzzShapingErrorKind::ShapingFailed;
        error->shaping_error = std::move(shaping_error);
        set_shaping_failure_message(error);
        return false;
    }

    stats->shaping = shaping_stats;
    stats->shaping_completed = true;
    return true;
}

} // namespace zevryon::text
