#pragma once

#include "catalog_harfbuzz_shaper.hpp"
#include "prepared_harfbuzz_face_cache.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace zevryon::text {

struct CachedCatalogHarfBuzzShapingRequest {
    const CatalogFontFaceBinding* binding{nullptr};
    PreparedHarfBuzzFaceCache* prepared_face_cache{nullptr};
    std::span<const DecodedCodePoint> codepoints;
    std::span<const GraphemeBoundary> grapheme_boundaries;
    std::uint32_t first_cluster{0};
    std::uint32_t cluster_limit{0};
    ScriptId script{ScriptId::Zyyy};
    ShapingDirection direction{ShapingDirection::LeftToRight};
    std::string_view language{"und"};
    std::span<const ShapingFeature> features;
    std::span<const ShapingVariation> variations;
    std::int32_t x_scale{0};
    std::int32_t y_scale{0};
    bool beginning_of_text{false};
    bool end_of_text{false};
    bool produce_unsafe_to_concat{true};
};

enum class CachedCatalogHarfBuzzShapingErrorKind : std::uint8_t {
    None = 0,
    InvalidArgument,
    FaceCacheFailed,
    ShapingFailed
};

struct CachedCatalogHarfBuzzShapingError {
    CachedCatalogHarfBuzzShapingErrorKind kind{
        CachedCatalogHarfBuzzShapingErrorKind::None};
    PreparedHarfBuzzFaceCacheError face_cache_error;
    BoundCatalogHarfBuzzShapingError shaping_error;
    std::string message;
};

struct CachedCatalogHarfBuzzShapingStats {
    PreparedHarfBuzzFaceCacheStats face_cache;
    BoundCatalogHarfBuzzShapingStats shaping;
    bool face_acquired{false};
    bool shaping_completed{false};
};

const char* cached_catalog_harfbuzz_shaping_error_kind_name(
    CachedCatalogHarfBuzzShapingErrorKind kind) noexcept;

// Acquires one immutable prepared face from the bounded single-flight cache and
// shapes one already-segmented run through the prepared catalog path. The
// binding is copied locally before cache work, and the acquired face remains
// retained through the complete synchronous shaping call. No platform locator,
// file I/O, content hash, SFNT verification, hb_blob_create, or hb_face_create
// occurs on a resident cache hit. Glyph output is empty after every failure.
bool shape_cached_catalog_harfbuzz_segment(
    const CachedCatalogHarfBuzzShapingRequest& request,
    ShapedGlyphRun* output,
    CachedCatalogHarfBuzzShapingStats* stats,
    CachedCatalogHarfBuzzShapingError* error) noexcept;

} // namespace zevryon::text
