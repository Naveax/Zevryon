#pragma once

#include "grapheme_segmenter.hpp"
#include "unicode_script.hpp"
#include "unicode_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zevryon::text {

enum class ShapingDirection : std::uint8_t {
    LeftToRight = 0,
    RightToLeft,
    TopToBottom,
    BottomToTop
};

struct ShapingFeature {
    std::uint32_t tag{0};
    std::uint32_t value{0};

    bool operator==(const ShapingFeature&) const noexcept = default;
};

struct ShapingVariation {
    std::uint32_t tag{0};
    float value{0.0F};

    bool operator==(const ShapingVariation&) const noexcept = default;
};

struct HarfBuzzShapingRequest {
    std::span<const std::byte> font_bytes;
    std::uint32_t face_index{0};
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

enum ShapedGlyphFlags : std::uint32_t {
    kShapedGlyphUnsafeToBreak = 1U << 0U,
    kShapedGlyphUnsafeToConcat = 1U << 1U,
    kShapedGlyphSafeToInsertTatweel = 1U << 2U
};

struct ShapedGlyph {
    std::uint32_t glyph_id{0};
    std::uint32_t cluster_index{0};
    std::int32_t x_advance{0};
    std::int32_t y_advance{0};
    std::int32_t x_offset{0};
    std::int32_t y_offset{0};
    std::uint32_t flags{0};

    bool operator==(const ShapedGlyph&) const noexcept = default;
};

static_assert(
    sizeof(ShapedGlyph) == 28U,
    "shaped glyph records must remain within the Z2 memory contract");

struct ShapedGlyphRun {
    explicit ShapedGlyphRun(std::pmr::memory_resource* resource);

    std::pmr::vector<ShapedGlyph> glyphs;
    std::uint32_t first_cluster{0};
    std::uint32_t cluster_limit{0};
    std::uint32_t face_index{0};
    ScriptId script{ScriptId::Zyyy};
    ShapingDirection direction{ShapingDirection::LeftToRight};
    std::uint16_t reserved{0};
    std::int32_t x_scale{0};
    std::int32_t y_scale{0};

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;
};

enum class HarfBuzzShapingErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    IndexOverflow,
    InvalidFontData,
    BackendAllocationFailed,
    ShapingFailed,
    InvalidBackendOutput,
    OutputBudgetExceeded
};

struct HarfBuzzShapingError {
    HarfBuzzShapingErrorKind kind{HarfBuzzShapingErrorKind::None};
    std::size_t input_index{0};
    std::string message;
};

struct HarfBuzzShapingStats {
    std::uint64_t font_bytes{0};
    std::uint64_t input_codepoints{0};
    std::uint64_t input_clusters{0};
    std::uint64_t output_glyphs{0};
    std::uint64_t missing_glyphs{0};
    std::uint64_t unsafe_to_break_glyphs{0};
    std::uint64_t unsafe_to_concat_glyphs{0};
    std::uint64_t safe_to_insert_tatweel_glyphs{0};
    std::int64_t total_x_advance{0};
    std::int64_t total_y_advance{0};
    std::uint64_t maximum_absolute_offset{0};
    std::uint32_t glyph_count_before_shaping{0};
    std::uint32_t units_per_em{0};
};

const char* harfbuzz_shaping_error_kind_name(
    HarfBuzzShapingErrorKind kind) noexcept;
const char* shaping_direction_name(ShapingDirection direction) noexcept;

// Shapes one already-segmented font/script/direction run. The caller owns and
// must keep font_bytes alive for the duration of this call. The output is
// published only after HarfBuzz succeeds, all clusters are validated, and one
// exact reserve in the caller-provided memory resource succeeds.
bool shape_harfbuzz_segment(
    const HarfBuzzShapingRequest& request,
    ShapedGlyphRun* output,
    HarfBuzzShapingStats* stats,
    HarfBuzzShapingError* error) noexcept;

} // namespace zevryon::text
