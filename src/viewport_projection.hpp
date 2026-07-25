#pragma once

#include "caret_boundary_map.hpp"
#include "line_box_layout.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <vector>

namespace zevryon::text {

enum ViewportLineFlags : std::uint32_t {
    kViewportLineBeforeViewport = 1U << 0U,
    kViewportLineAfterViewport = 1U << 1U,
    kViewportLineContainsSelection = 1U << 2U,
    kViewportLineContainsRtl = 1U << 3U
};

enum ViewportFragmentFlags : std::uint32_t {
    kViewportFragmentRtl = 1U << 0U,
    kViewportFragmentL1Adjusted = 1U << 1U,
    kViewportFragmentContainsX9Only = 1U << 2U,
    kViewportFragmentBeforeInlineViewport = 1U << 3U,
    kViewportFragmentAfterInlineViewport = 1U << 4U
};

enum ViewportCaretFlags : std::uint32_t {
    kViewportCaretRtl = 1U << 0U,
    kViewportCaretLineEdge = 1U << 1U,
    kViewportCaretFragmentEdge = 1U << 2U,
    kViewportCaretTextEdge = 1U << 3U
};

enum ViewportSelectionRectFlags : std::uint32_t {
    kViewportSelectionRtl = 1U << 0U,
    kViewportSelectionStartsInsideFragment = 1U << 1U,
    kViewportSelectionEndsInsideFragment = 1U << 2U
};

struct ViewportLineRecord final {
    std::int64_t viewport_block_start{0};
    std::int64_t viewport_baseline{0};
    std::uint64_t block_size{0};
    std::uint64_t inline_advance{0};
    std::uint32_t source_line_index{0};
    std::uint32_t first_fragment_rect{0};
    std::uint32_t fragment_rect_count{0};
    std::uint32_t first_caret{0};
    std::uint32_t caret_count{0};
    std::uint32_t first_selection_rect{0};
    std::uint32_t selection_rect_count{0};
    std::uint32_t flags{0};

    bool operator==(const ViewportLineRecord&) const noexcept = default;
};

static_assert(
    sizeof(ViewportLineRecord) == 64U,
    "viewport line records must remain within the Z2 memory contract");

struct ViewportFragmentRect final {
    std::int64_t viewport_inline_start{0};
    std::int64_t viewport_block_start{0};
    std::uint64_t inline_size{0};
    std::uint64_t block_size{0};
    std::uint32_t source_fragment_index{0};
    std::uint32_t first_cluster{0};
    std::uint32_t cluster_limit{0};
    std::uint32_t flags{0};

    bool operator==(const ViewportFragmentRect&) const noexcept = default;
};

static_assert(
    sizeof(ViewportFragmentRect) == 48U,
    "viewport fragment rectangles must remain within the Z2 memory contract");

struct ViewportCaretEdge final {
    std::int64_t viewport_inline_position{0};
    std::int64_t viewport_block_start{0};
    std::uint64_t block_size{0};
    std::uint32_t boundary_index{0};
    std::uint32_t source_fragment_index{0};
    std::uint32_t flags{0};
    std::uint32_t reserved{0};

    bool operator==(const ViewportCaretEdge&) const noexcept = default;
};

static_assert(
    sizeof(ViewportCaretEdge) == 40U,
    "viewport caret records must remain within the Z2 memory contract");

struct ViewportSelectionRect final {
    std::int64_t viewport_inline_start{0};
    std::int64_t viewport_block_start{0};
    std::uint64_t inline_size{0};
    std::uint64_t block_size{0};
    std::uint32_t source_line_index{0};
    std::uint32_t source_fragment_index{0};
    std::uint32_t flags{0};
    std::uint32_t reserved{0};

    bool operator==(const ViewportSelectionRect&) const noexcept = default;
};

static_assert(
    sizeof(ViewportSelectionRect) == 48U,
    "viewport selection rectangles must remain within the Z2 memory contract");

class ViewportProjection final {
public:
    explicit ViewportProjection(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    ViewportProjection(const ViewportProjection&) = delete;
    ViewportProjection& operator=(const ViewportProjection&) = delete;
    ViewportProjection(ViewportProjection&&) noexcept = default;
    ViewportProjection& operator=(ViewportProjection&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    std::uint64_t viewport_inline_start{0};
    std::uint64_t viewport_block_start{0};
    std::uint64_t document_block_extent{0};
    std::pmr::vector<ViewportLineRecord> lines;
    std::pmr::vector<ViewportFragmentRect> fragment_rects;
    std::pmr::vector<ViewportCaretEdge> carets;
    std::pmr::vector<ViewportSelectionRect> selection_rects;
};

struct ViewportProjectionLimits final {
    std::uint32_t maximum_lines{0};
    std::uint32_t maximum_fragment_rects{0};
    std::uint32_t maximum_carets{0};
    std::uint32_t maximum_selection_rects{0};
};

struct ViewportSelectionRange final {
    std::uint32_t first_boundary{0};
    std::uint32_t boundary_limit{0};
    bool enabled{false};
};

struct ViewportProjectionRequest final {
    const LineBoxLayout* line_boxes{nullptr};
    const LineFragmentLayout* fragment_layout{nullptr};
    const MultiRunShapedText* shaped_text{nullptr};
    const GlyphClusterMap* cluster_map{nullptr};
    const CaretBoundaryMap* caret_boundaries{nullptr};
    std::uint64_t viewport_inline_start{0};
    std::uint64_t viewport_inline_size{0};
    std::uint64_t viewport_block_start{0};
    std::uint64_t viewport_block_size{0};
    std::uint64_t inline_overscan{0};
    std::uint64_t block_overscan{0};
    ViewportSelectionRange selection;
    ViewportProjectionLimits limits;
};

enum class ViewportProjectionErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    TopologyViolation,
    ArithmeticOverflow,
    ProjectionLimitExceeded,
    OutputBudgetExceeded,
    AggregateOverflow
};

struct ViewportProjectionError final {
    ViewportProjectionErrorKind kind{ViewportProjectionErrorKind::None};
    std::size_t line_index{0};
    std::size_t fragment_index{0};
    std::uint32_t cluster_index{0};
    std::string message;
};

struct ViewportProjectionStats final {
    std::uint64_t input_lines{0};
    std::uint64_t input_fragments{0};
    std::uint64_t input_clusters{0};
    std::uint64_t first_source_line{0};
    std::uint64_t source_line_limit{0};
    std::uint64_t output_lines{0};
    std::uint64_t output_fragment_rects{0};
    std::uint64_t output_carets{0};
    std::uint64_t output_selection_rects{0};
    std::uint64_t glyph_groups{0};
    std::uint64_t unsafe_caret_boundaries_skipped{0};
    std::uint64_t rtl_fragment_rects{0};
    std::uint64_t lines_before_viewport{0};
    std::uint64_t lines_after_viewport{0};
    std::uint64_t maximum_fragments_per_line{0};
    std::uint64_t maximum_carets_per_line{0};
    std::uint64_t maximum_selection_rects_per_line{0};
};

const char* viewport_projection_error_kind_name(
    ViewportProjectionErrorKind kind) noexcept;

// Projects only the block-window and inline-window intersection plus configured
// overscan. Every output category has an explicit caller limit. Glyphs and the
// upstream line/fragment arrays are referenced, never copied. Output is empty
// after every failure.
bool build_viewport_projection(
    const ViewportProjectionRequest& request,
    ViewportProjection* output,
    ViewportProjectionStats* stats,
    ViewportProjectionError* error) noexcept;

enum class ViewportHitTestBias : std::uint8_t {
    Nearest = 0,
    TowardVisualStart,
    TowardVisualEnd
};

enum ViewportHitTestFlags : std::uint32_t {
    kViewportHitClampedInline = 1U << 0U,
    kViewportHitClampedBlock = 1U << 1U
};

struct ViewportHitTestResult final {
    std::uint32_t source_line_index{0};
    std::uint32_t source_fragment_index{0};
    std::uint32_t boundary_index{0};
    std::uint32_t flags{0};
    std::uint64_t inline_distance{0};
    std::uint64_t block_distance{0};
};

// Allocation-free binary/nearest search over the bounded projection. The result
// always names a certified safe logical caret boundary.
bool hit_test_viewport_projection(
    const ViewportProjection& projection,
    std::int64_t viewport_inline_position,
    std::int64_t viewport_block_position,
    ViewportHitTestBias bias,
    ViewportHitTestResult* output) noexcept;

} // namespace zevryon::text
