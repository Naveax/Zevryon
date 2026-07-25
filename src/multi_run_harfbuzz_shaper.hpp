#pragma once

#include "cached_catalog_harfbuzz_shaper.hpp"
#include "shaping_run_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zevryon::text {

struct MultiRunShapedSegment final {
    explicit MultiRunShapedSegment(std::pmr::memory_resource* glyph_resource);

    MultiRunShapedSegment(const MultiRunShapedSegment&) = delete;
    MultiRunShapedSegment& operator=(const MultiRunShapedSegment&) = delete;
    MultiRunShapedSegment(MultiRunShapedSegment&&) noexcept = default;
    MultiRunShapedSegment& operator=(MultiRunShapedSegment&&) = delete;

    ShapingRunBoundary run;
    ShapedGlyphRun glyphs;
};

class MultiRunShapedText final {
public:
    explicit MultiRunShapedText(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());
    MultiRunShapedText(
        std::pmr::memory_resource* metadata_resource,
        std::pmr::memory_resource* glyph_resource);

    MultiRunShapedText(const MultiRunShapedText&) = delete;
    MultiRunShapedText& operator=(const MultiRunShapedText&) = delete;
    MultiRunShapedText(MultiRunShapedText&&) noexcept = default;
    MultiRunShapedText& operator=(MultiRunShapedText&&) = delete;

    std::pmr::memory_resource* metadata_resource() const noexcept;
    std::pmr::memory_resource* glyph_resource() const noexcept;
    void release() noexcept;

    // One segment per logical shaping run. Each segment retains the exact
    // allocation produced by the existing single-run HarfBuzz boundary; glyphs
    // are not flattened or copied into a second full-size buffer.
    std::pmr::vector<MultiRunShapedSegment> segments;

private:
    std::pmr::memory_resource* glyph_resource_;
};

struct MultiRunCatalogHarfBuzzShapingRequest {
    const ShapingRunPlan* plan{nullptr};

    // Valid immutable bindings sorted strictly by face_id. Every binding must
    // belong to the same catalog generation. Extra unused bindings are allowed.
    std::span<const CatalogFontFaceBinding> bindings;
    PreparedHarfBuzzFaceCache* prepared_face_cache{nullptr};

    std::span<const DecodedCodePoint> codepoints;
    std::span<const GraphemeBoundary> grapheme_boundaries;
    std::string_view language{"und"};
    std::span<const ShapingFeature> features;
    std::span<const ShapingVariation> variations;
    std::int32_t x_scale{0};
    std::int32_t y_scale{0};
    bool produce_unsafe_to_concat{true};
};

enum class MultiRunCatalogHarfBuzzShapingErrorKind : std::uint8_t {
    None = 0,
    InvalidArgument,
    InvalidPlan,
    InvalidBindingTable,
    MissingFontRun,
    FaceBindingNotFound,
    GenerationMismatch,
    MetadataBudgetExceeded,
    SegmentShapingFailed,
    AggregateOverflow
};

struct MultiRunCatalogHarfBuzzShapingError {
    MultiRunCatalogHarfBuzzShapingErrorKind kind{
        MultiRunCatalogHarfBuzzShapingErrorKind::None};
    std::size_t run_index{0};
    std::uint32_t cluster_index{0};
    FontFaceId face_id{kInvalidFontFaceId};
    CachedCatalogHarfBuzzShapingError segment_error;
    std::string message;
};

struct MultiRunCatalogHarfBuzzShapingStats {
    PreparedHarfBuzzFaceCacheStats cache_before;
    PreparedHarfBuzzFaceCacheStats cache_after;
    FontGenerationFingerprint generation_fingerprint{};
    std::uint64_t generation_id{0};
    std::uint64_t input_runs{0};
    std::uint64_t completed_runs{0};
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
    std::uint64_t left_to_right_runs{0};
    std::uint64_t right_to_left_runs{0};
    std::uint64_t distinct_bound_faces{0};
    std::size_t maximum_run_glyphs{0};
};

const char* multi_run_catalog_harfbuzz_shaping_error_kind_name(
    MultiRunCatalogHarfBuzzShapingErrorKind kind) noexcept;

// Executes every logical run through the bounded cache-backed prepared-face
// path. The outer segment table reserves exactly plan.run_count entries once;
// each nested glyph vector preserves the existing exact single-run allocation.
// Missing-font runs and binding/generation inconsistencies fail closed. Output
// is empty after every failure.
bool shape_multi_run_catalog_harfbuzz(
    const MultiRunCatalogHarfBuzzShapingRequest& request,
    MultiRunShapedText* output,
    MultiRunCatalogHarfBuzzShapingStats* stats,
    MultiRunCatalogHarfBuzzShapingError* error) noexcept;

} // namespace zevryon::text
