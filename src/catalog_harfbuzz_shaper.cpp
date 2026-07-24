#include "catalog_harfbuzz_shaper.hpp"

#include <memory>
#include <utility>

namespace zevryon::text {
namespace {

void clear_resource_error(CatalogFontResourceError* error) noexcept {
    if (error != nullptr) {
        error->kind = CatalogFontResourceErrorKind::None;
        error->locator_error = {};
        error->file_error = {};
        error->message.clear();
    }
}

bool fail_binding_argument(
    const char* message,
    CatalogFontResourceError* error) noexcept {
    if (error != nullptr) {
        error->kind = CatalogFontResourceErrorKind::InvalidArgument;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

void clear_shaping_error(BoundCatalogHarfBuzzShapingError* error) noexcept {
    if (error != nullptr) {
        error->kind = BoundCatalogHarfBuzzShapingErrorKind::None;
        error->shaping_error = {};
        error->message.clear();
    }
}

bool fail_shaping(
    BoundCatalogHarfBuzzShapingErrorKind kind,
    const char* message,
    BoundCatalogHarfBuzzShapingError* error) noexcept {
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

void set_nested_shaping_message(
    BoundCatalogHarfBuzzShapingError* error) noexcept {
    try {
        error->message = "bound catalog HarfBuzz shaping failed: ";
        error->message += harfbuzz_shaping_error_kind_name(
            error->shaping_error.kind);
    } catch (...) {
        error->message.clear();
    }
}

} // namespace

bool CatalogFontFaceBinding::valid() const noexcept {
    return generation_ != nullptr && resource_ != nullptr &&
        face_id_ != kInvalidFontFaceId &&
        static_cast<std::size_t>(face_id_) <
            generation_->discovery_records().size() &&
        resource_->view().valid() && !resource_->bytes().empty() &&
        resource_->bytes().data() == resource_->view().bytes().data() &&
        resource_->accounting_clean() && resource_->within_hard_limit() &&
        generation_->accounting_clean() && generation_->within_hard_limits();
}

std::uint64_t CatalogFontFaceBinding::generation_id() const noexcept {
    return generation_ != nullptr ? generation_->generation_id() : 0U;
}

FontFaceId CatalogFontFaceBinding::face_id() const noexcept {
    return face_id_;
}

std::uint64_t CatalogFontFaceBinding::resource_id() const noexcept {
    return resource_ != nullptr ? resource_->resource_id() : 0U;
}

const std::shared_ptr<const FontCatalogGeneration>&
CatalogFontFaceBinding::generation() const noexcept {
    return generation_;
}

const std::shared_ptr<const VerifiedFontResource>&
CatalogFontFaceBinding::resource() const noexcept {
    return resource_;
}

bool bind_catalog_font_face(
    std::shared_ptr<const FontCatalogGeneration> generation,
    FontFaceId face_id,
    std::size_t staging_hard_limit,
    VerifiedFontResourceCache* cache,
    CatalogFontFaceBinding* output,
    CatalogFontResourceStats* stats,
    CatalogFontResourceError* error) noexcept {
    if (output != nullptr) {
        *output = CatalogFontFaceBinding{};
    }
    if (stats != nullptr) {
        *stats = {};
    }
    clear_resource_error(error);

    if (!generation || cache == nullptr || output == nullptr ||
        stats == nullptr || error == nullptr || staging_hard_limit == 0U) {
        return fail_binding_argument(
            "generation, cache, positive staging limit, output, stats, and error are required",
            error);
    }

    std::shared_ptr<const VerifiedFontResource> resource;
    if (!resolve_catalog_font_resource(
            generation,
            face_id,
            staging_hard_limit,
            cache,
            &resource,
            stats,
            error)) {
        return false;
    }

    CatalogFontFaceBinding candidate;
    candidate.generation_ = std::move(generation);
    candidate.resource_ = std::move(resource);
    candidate.face_id_ = face_id;
    if (!candidate.valid()) {
        return fail_binding_argument(
            "resolved catalog face did not produce a valid immutable binding",
            error);
    }

    *output = std::move(candidate);
    return true;
}

const char* bound_catalog_harfbuzz_shaping_error_kind_name(
    BoundCatalogHarfBuzzShapingErrorKind kind) noexcept {
    switch (kind) {
    case BoundCatalogHarfBuzzShapingErrorKind::None:
        return "none";
    case BoundCatalogHarfBuzzShapingErrorKind::InvalidArgument:
        return "invalid_argument";
    case BoundCatalogHarfBuzzShapingErrorKind::InvalidBinding:
        return "invalid_binding";
    case BoundCatalogHarfBuzzShapingErrorKind::ShapingFailed:
        return "shaping_failed";
    }
    return "unknown";
}

bool shape_bound_catalog_harfbuzz_segment(
    const BoundCatalogHarfBuzzShapingRequest& request,
    ShapedGlyphRun* output,
    BoundCatalogHarfBuzzShapingStats* stats,
    BoundCatalogHarfBuzzShapingError* error) noexcept {
    if (output != nullptr) {
        output->release();
    }
    if (stats != nullptr) {
        *stats = {};
    }
    clear_shaping_error(error);

    if (output == nullptr || stats == nullptr || error == nullptr ||
        request.binding == nullptr) {
        return fail_shaping(
            BoundCatalogHarfBuzzShapingErrorKind::InvalidArgument,
            "binding, output, stats, and error are required",
            error);
    }
    if (!request.binding->valid()) {
        return fail_shaping(
            BoundCatalogHarfBuzzShapingErrorKind::InvalidBinding,
            "catalog font-face binding is not valid",
            error);
    }

    const std::shared_ptr<const FontCatalogGeneration> generation =
        request.binding->generation();
    const std::shared_ptr<const VerifiedFontResource> resource =
        request.binding->resource();
    if (!generation || !resource) {
        return fail_shaping(
            BoundCatalogHarfBuzzShapingErrorKind::InvalidBinding,
            "catalog font-face binding lost retained ownership",
            error);
    }

    stats->generation_id = generation->generation_id();
    stats->face_id = request.binding->face_id();
    stats->resource_id = resource->resource_id();

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
    shaping_request.verified_font_resource = resource;

    HarfBuzzShapingStats shaping_stats;
    HarfBuzzShapingError shaping_error;
    if (!shape_harfbuzz_segment(
            shaping_request,
            output,
            &shaping_stats,
            &shaping_error)) {
        stats->shaping = shaping_stats;
        error->kind = BoundCatalogHarfBuzzShapingErrorKind::ShapingFailed;
        error->shaping_error = std::move(shaping_error);
        set_nested_shaping_message(error);
        return false;
    }

    stats->shaping = shaping_stats;
    stats->shaping_completed = true;
    return true;
}

} // namespace zevryon::text
