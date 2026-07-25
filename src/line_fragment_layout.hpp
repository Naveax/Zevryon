#pragma once

#include "bidi_visual.hpp"
#include "grapheme_segmenter.hpp"
#include "line_selection.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

enum VisualLineLayoutFlags : std::uint32_t {
    kVisualLineContainsRtl = 1U << 16U,
    kVisualLineL1Adjusted = 1U << 17U,
    kVisualLineContainsX9Only = 1U << 18U
};

enum InlineLayoutFragmentFlags : std::uint8_t {
    kInlineFragmentGlyphRunRtl = 1U << 0U,
    kInlineFragmentL1Adjusted = 1U << 1U,
    kInlineFragmentContainsX9Only = 1U << 2U
};

struct VisualLineLayoutRecord final {
    std::uint64_t inline_advance{0};
    std::uint32_t first_fragment{0};
    std::uint32_t fragment_count{0};
    std::uint32_t cluster_limit{0};
    std::uint32_t flags{0};

    bool operator==(const VisualLineLayoutRecord&) const noexcept = default;
};

static_assert(
    sizeof(VisualLineLayoutRecord) == 24U,
    "visual-line records must remain within the Z2 memory contract");

struct InlineLayoutFragment final {
    std::uint64_t inline_offset{0};
    std::uint64_t inline_advance{0};
    std::uint32_t segment_index{0};
    std::uint32_t first_cluster{0};
    std::uint32_t cluster_limit{0};
    std::uint8_t bidi_level{0};
    std::uint8_t flags{0};
    std::uint16_t reserved{0};

    bool operator==(const InlineLayoutFragment&) const noexcept = default;
};

static_assert(
    sizeof(InlineLayoutFragment) == 32U,
    "inline-layout fragments must remain within the Z2 memory contract");

class LineFragmentLayout final {
public:
    explicit LineFragmentLayout(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    LineFragmentLayout(const LineFragmentLayout&) = delete;
    LineFragmentLayout& operator=(const LineFragmentLayout&) = delete;
    LineFragmentLayout(LineFragmentLayout&&) noexcept = default;
    LineFragmentLayout& operator=(LineFragmentLayout&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    // Lines remain in logical block order. Each line references a contiguous
    // fragment slice already ordered from visual inline start to inline end.
    std::pmr::vector<VisualLineLayoutRecord> lines;
    std::pmr::vector<InlineLayoutFragment> fragments;
};

struct LineFragmentLayoutRequest final {
    std::span<const GraphemeBoundary> grapheme_boundaries;
    std::span<const BidiExplicitUnit> bidi_units;
    const BidiSequenceTopology* bidi_topology{nullptr};
    std::span<const std::uint8_t> implicit_levels;
    std::uint8_t paragraph_level{0};
    const MultiRunShapedText* shaped_text{nullptr};
    const GlyphClusterMap* cluster_map{nullptr};
    const LineSelection* line_selection{nullptr};
    std::uint32_t cluster_count{0};
};

enum class LineFragmentLayoutErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    TopologyViolation,
    ClusterMappingViolation,
    MixedClusterDirection,
    UnsafeFragmentBoundary,
    BidiVisualFailure,
    AdvanceOverflow,
    OutputBudgetExceeded,
    AggregateOverflow
};

struct LineFragmentLayoutError final {
    LineFragmentLayoutErrorKind kind{LineFragmentLayoutErrorKind::None};
    std::size_t line_index{0};
    std::size_t active_index{0};
    std::uint32_t cluster_index{0};
    std::uint32_t segment_index{0};
    BidiVisualError bidi_error;
    std::string message;
};

struct LineFragmentLayoutStats final {
    BidiVisualStats bidi_visual;
    std::uint64_t input_lines{0};
    std::uint64_t input_segments{0};
    std::uint64_t input_glyphs{0};
    std::uint64_t input_clusters{0};
    std::uint64_t input_active_units{0};
    std::uint64_t zero_active_lines{0};
    std::uint64_t zero_active_clusters{0};
    std::uint64_t same_direction_mixed_level_clusters{0};
    std::uint64_t l1_adjusted_clusters{0};
    std::uint64_t output_lines{0};
    std::uint64_t output_fragments{0};
    std::uint64_t rtl_fragments{0};
    std::uint64_t l1_adjusted_fragments{0};
    std::uint64_t x9_only_fragments{0};
    std::uint64_t l2_reversal_spans{0};
    std::uint64_t l2_reversed_fragments{0};
    std::uint64_t total_inline_advance{0};
    std::uint64_t maximum_line_advance{0};
    std::uint64_t maximum_fragments_per_line{0};
    std::uint8_t maximum_fragment_level{0};
};

const char* line_fragment_layout_error_kind_name(
    LineFragmentLayoutErrorKind kind) noexcept;

// Converts selected logical lines into compact visual-order fragment slices.
// UAX #9 L1-adjusted scalar levels are resolved through the certified visual
// stage, then compressed at grapheme and HarfBuzz segment boundaries. UAX #9
// L2 is applied to fragment records; L3 remains inside grapheme-atomic shaped
// runs. Every split must be a safe glyph-group boundary. Output is empty after
// every failure.
bool build_line_fragment_layout(
    const LineFragmentLayoutRequest& request,
    LineFragmentLayout* output,
    LineFragmentLayoutStats* stats,
    LineFragmentLayoutError* error) noexcept;

} // namespace zevryon::text
