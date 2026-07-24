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

class CatalogFontFaceBinding final {
public:
    CatalogFontFaceBinding() = default;

    bool valid() const noexcept;
    std::uint64_t generation_id() const noexcept;
    FontFaceId face_id() const noexcept;
    std::uint64_t resource_id() const noexcept;

    const std::shared_ptr<const FontCatalogGeneration>& generation()
        const noexcept;
    const std::shared_ptr<const VerifiedFontResource>& resource()
        const noexcept;

private:
    friend bool bind_catalog_font_face(
        std::shared_ptr<const FontCatalogGeneration>,
        FontFaceId,
        std::size_t,
        VerifiedFontResourceCache*,
        CatalogFontFaceBinding*,
        CatalogFontResourceStats*,
        CatalogFontResourceError*) noexcept;

    std::shared_ptr<const FontCatalogGeneration> generation_;
    std::shared_ptr<const VerifiedFontResource> resource_;
    FontFaceId face_id_{kInvalidFontFaceId};
};

// Performs the cold catalog/platform/file/content-resolution stage once and
// publishes one immutable binding only after the verified resource is ready.
// A failed rebind clears any previously valid binding.
bool bind_catalog_font_face(
    std::shared_ptr<const FontCatalogGeneration> generation,
    FontFaceId face_id,
    std::size_t staging_hard_limit,
    VerifiedFontResourceCache* cache,
    CatalogFontFaceBinding* output,
    CatalogFontResourceStats* stats,
    CatalogFontResourceError* error) noexcept;

struct BoundCatalogHarfBuzzShapingRequest {
    const CatalogFontFaceBinding* binding{nullptr};
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

enum class BoundCatalogHarfBuzzShapingErrorKind : std::uint8_t {
    None = 0,
    InvalidArgument,
    InvalidBinding,
    ShapingFailed
};

struct BoundCatalogHarfBuzzShapingError {
    BoundCatalogHarfBuzzShapingErrorKind kind{
        BoundCatalogHarfBuzzShapingErrorKind::None};
    HarfBuzzShapingError shaping_error;
    std::string message;
};

struct BoundCatalogHarfBuzzShapingStats {
    std::uint64_t generation_id{0};
    FontFaceId face_id{kInvalidFontFaceId};
    std::uint64_t resource_id{0};
    HarfBuzzShapingStats shaping;
    bool shaping_completed{false};
};

const char* bound_catalog_harfbuzz_shaping_error_kind_name(
    BoundCatalogHarfBuzzShapingErrorKind kind) noexcept;

// Hot path: performs no platform-identity parsing, filesystem access, content
// hashing, verified-resource cache lookup, or font verification. The immutable
// binding's generation and resource handles are retained locally for the full
// synchronous HarfBuzz call. Glyph output is empty after every failure.
bool shape_bound_catalog_harfbuzz_segment(
    const BoundCatalogHarfBuzzShapingRequest& request,
    ShapedGlyphRun* output,
    BoundCatalogHarfBuzzShapingStats* stats,
    BoundCatalogHarfBuzzShapingError* error) noexcept;

} // namespace zevryon::text
