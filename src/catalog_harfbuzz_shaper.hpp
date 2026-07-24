#pragma once

#include "catalog_font_resource_resolver.hpp"
#include "harfbuzz_shaper.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace zevryon::text {

struct CatalogHarfBuzzShapingRequest {
    std::shared_ptr<const FontCatalogGeneration> generation;
    FontFaceId face_id{kInvalidFontFaceId};
    std::size_t staging_hard_limit{0};
    VerifiedFontResourceCache* cache{nullptr};

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

enum class CatalogHarfBuzzShapingErrorKind : std::uint8_t {
    None = 0,
    InvalidArgument,
    ResourceResolutionFailed,
    ShapingFailed
};

struct CatalogHarfBuzzShapingError {
    CatalogHarfBuzzShapingErrorKind kind{
        CatalogHarfBuzzShapingErrorKind::None};
    CatalogFontResourceError resource_error;
    HarfBuzzShapingError shaping_error;
    std::string message;
};

struct CatalogHarfBuzzShapingStats {
    CatalogFontResourceStats resource;
    HarfBuzzShapingStats shaping;
    bool resource_resolved{false};
    bool shaping_completed{false};
};

const char* catalog_harfbuzz_shaping_error_kind_name(
    CatalogHarfBuzzShapingErrorKind kind) noexcept;

// Resolves one immutable catalog face through the bounded verified-resource
// cache and shapes one already-segmented run through the existing retained
// HarfBuzz path. The catalog generation and verified resource remain retained
// for the complete synchronous call. Glyph output is empty after every failure.
bool shape_catalog_harfbuzz_segment(
    const CatalogHarfBuzzShapingRequest& request,
    ShapedGlyphRun* output,
    CatalogHarfBuzzShapingStats* stats,
    CatalogHarfBuzzShapingError* error) noexcept;

} // namespace zevryon::text
