#pragma once

#include "viewport_projection.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

enum class TextPaintCommandKind : std::uint32_t {
    SelectionRect = 0,
    GlyphBatch,
    CaretRect
};

enum TextPaintCommandFlags : std::uint32_t {
    kTextPaintCommandBeforeViewport = 1U << 0U,
    kTextPaintCommandAfterViewport = 1U << 1U
};

enum TextPaintRectFlags : std::uint32_t {
    kTextPaintRectSelection = 1U << 0U,
    kTextPaintRectCaret = 1U << 1U,
    kTextPaintRectRtl = 1U << 2U,
    kTextPaintRectStartsInsideFragment = 1U << 3U,
    kTextPaintRectEndsInsideFragment = 1U << 4U
};

enum TextPaintGlyphBatchFlags : std::uint32_t {
    kTextPaintGlyphBatchRtl = 1U << 0U,
    kTextPaintGlyphBatchL1Adjusted = 1U << 1U,
    kTextPaintGlyphBatchBeforeViewport = 1U << 2U,
    kTextPaintGlyphBatchAfterViewport = 1U << 3U,
    kTextPaintGlyphBatchBeforeInlineViewport = 1U << 4U,
    kTextPaintGlyphBatchAfterInlineViewport = 1U << 5U,
    kTextPaintGlyphBatchCoalesced = 1U << 6U
};

struct TextPaintCommandRecord final {
    TextPaintCommandKind kind{TextPaintCommandKind::SelectionRect};
    std::uint32_t payload_index{0};
    std::uint32_t clip_index{0};
    std::uint32_t flags{0};

    bool operator==(const TextPaintCommandRecord&) const noexcept = default;
};

static_assert(
    sizeof(TextPaintCommandRecord) == 16U,
    "paint command records must remain within the Z2 memory contract");

struct TextPaintClipRect final {
    std::int64_t viewport_inline_start{0};
    std::int64_t viewport_block_start{0};
    std::uint64_t inline_size{0};
    std::uint64_t block_size{0};

    bool operator==(const TextPaintClipRect&) const noexcept = default;
};

static_assert(
    sizeof(TextPaintClipRect) == 32U,
    "paint clip records must remain within the Z2 memory contract");

struct TextPaintFillRect final {
    std::int64_t viewport_inline_start{0};
    std::int64_t viewport_block_start{0};
    std::uint64_t inline_size{0};
    std::uint64_t block_size{0};
    std::uint32_t style_id{0};
    std::uint32_t source_line_index{0};
    std::uint32_t source_fragment_index{0};
    std::uint32_t flags{0};

    bool operator==(const TextPaintFillRect&) const noexcept = default;
};

static_assert(
    sizeof(TextPaintFillRect) == 48U,
    "paint fill-rect records must remain within the Z2 memory contract");

struct TextPaintGlyphBatch final {
    std::int64_t viewport_inline_origin{0};
    std::int64_t viewport_baseline{0};
    std::uint32_t segment_index{0};
    std::uint32_t first_glyph{0};
    std::uint32_t glyph_count{0};
    std::uint32_t style_id{0};
    FontFaceId face_id{kInvalidFontFaceId};
    std::int32_t x_scale{0};
    std::int32_t y_scale{0};
    std::uint32_t source_line_index{0};
    std::uint32_t first_source_fragment_index{0};
    std::uint32_t source_fragment_count{0};
    std::uint32_t flags{0};
    std::uint32_t reserved{0};

    bool operator==(const TextPaintGlyphBatch&) const noexcept = default;
};

static_assert(
    sizeof(TextPaintGlyphBatch) == 64U,
    "paint glyph-batch records must remain within the Z2 memory contract");

class TextPaintCommandStream final {
public:
    explicit TextPaintCommandStream(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    TextPaintCommandStream(const TextPaintCommandStream&) = delete;
    TextPaintCommandStream& operator=(const TextPaintCommandStream&) = delete;
    TextPaintCommandStream(TextPaintCommandStream&&) noexcept = default;
    TextPaintCommandStream& operator=(TextPaintCommandStream&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    // Commands are partitioned in strict paint order:
    // selection backgrounds, glyph batches, then the optional caret.
    std::pmr::vector<TextPaintClipRect> clips;
    std::pmr::vector<TextPaintCommandRecord> commands;
    std::pmr::vector<TextPaintGlyphBatch> glyph_batches;
    std::pmr::vector<TextPaintFillRect> fill_rects;
};

struct TextPaintCaretSelector final {
    std::uint32_t source_line_index{0};
    std::uint32_t source_fragment_index{0};
    std::uint32_t boundary_index{0};
    std::uint32_t reserved{0};
    std::uint64_t inline_size{1};
    bool enabled{false};
};

struct TextPaintCommandStreamLimits final {
    std::uint32_t maximum_commands{0};
    std::uint32_t maximum_glyph_batches{0};
    std::uint32_t maximum_fill_rects{0};
    std::uint64_t maximum_referenced_glyphs{0};
};

struct TextPaintCommandStreamRequest final {
    const ViewportProjection* projection{nullptr};
    const LineFragmentLayout* fragment_layout{nullptr};
    const MultiRunShapedText* shaped_text{nullptr};
    const GlyphClusterMap* cluster_map{nullptr};

    // Empty means every segment uses default_text_style_id. Otherwise this span
    // must contain exactly one immutable backend style handle per shaped segment.
    std::span<const std::uint32_t> segment_style_ids;
    std::uint32_t default_text_style_id{0};
    std::uint32_t selection_style_id{0};
    std::uint32_t caret_style_id{0};

    std::uint64_t clip_inline_size{0};
    std::uint64_t clip_block_size{0};
    bool paint_selection{true};
    TextPaintCaretSelector caret;
    TextPaintCommandStreamLimits limits;
};

enum class TextPaintCommandStreamErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    TopologyViolation,
    MissingGlyphSpan,
    AdvanceMismatch,
    CaretNotFound,
    ArithmeticOverflow,
    CommandLimitExceeded,
    OutputBudgetExceeded,
    AggregateOverflow
};

struct TextPaintCommandStreamError final {
    TextPaintCommandStreamErrorKind kind{
        TextPaintCommandStreamErrorKind::None};
    std::size_t line_index{0};
    std::size_t fragment_index{0};
    std::size_t glyph_index{0};
    std::uint32_t cluster_index{0};
    std::string message;
};

struct TextPaintCommandStreamStats final {
    std::uint64_t input_lines{0};
    std::uint64_t input_fragment_rects{0};
    std::uint64_t input_selection_rects{0};
    std::uint64_t input_carets{0};
    std::uint64_t output_clips{0};
    std::uint64_t output_commands{0};
    std::uint64_t output_glyph_batches{0};
    std::uint64_t output_fill_rects{0};
    std::uint64_t selection_commands{0};
    std::uint64_t caret_commands{0};
    std::uint64_t referenced_glyphs{0};
    std::uint64_t zero_area_selection_rects_skipped{0};
    std::uint64_t zero_glyph_fragments_skipped{0};
    std::uint64_t coalesced_fragments{0};
    std::uint64_t rtl_glyph_batches{0};
    std::uint64_t lines_before_viewport{0};
    std::uint64_t lines_after_viewport{0};
    std::uint64_t maximum_glyphs_per_batch{0};
    std::uint64_t maximum_glyph_batches_per_line{0};
};

const char* text_paint_command_stream_error_kind_name(
    TextPaintCommandStreamErrorKind kind) noexcept;

// Converts a bounded viewport projection into a backend-neutral command stream.
// Glyph payloads retain only shaped-segment and contiguous glyph-span references;
// glyph records, projection geometry, and font data are never copied. Output is
// empty after every failure.
bool build_text_paint_command_stream(
    const TextPaintCommandStreamRequest& request,
    TextPaintCommandStream* output,
    TextPaintCommandStreamStats* stats,
    TextPaintCommandStreamError* error) noexcept;

} // namespace zevryon::text
