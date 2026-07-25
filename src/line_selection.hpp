#pragma once

#include "caret_boundary_map.hpp"
#include "line_break_opportunity.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <vector>

namespace zevryon::text {

enum SelectedLineFlags : std::uint32_t {
    kSelectedLineSoftBreak = 1U << 0U,
    kSelectedLineMandatoryBreak = 1U << 1U,
    kSelectedLineOverflow = 1U << 2U,
    kSelectedLineTextEnd = 1U << 3U,
    kSelectedLineEmpty = 1U << 4U
};

struct SelectedLineRecord final {
    std::uint64_t inline_advance{0};
    std::uint32_t cluster_limit{0};
    std::uint32_t flags{0};

    bool operator==(const SelectedLineRecord&) const noexcept = default;
};

static_assert(
    sizeof(SelectedLineRecord) == 16U,
    "selected-line records must remain within the Z2 memory contract");

class LineSelection final {
public:
    explicit LineSelection(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    LineSelection(const LineSelection&) = delete;
    LineSelection& operator=(const LineSelection&) = delete;
    LineSelection(LineSelection&&) noexcept = default;
    LineSelection& operator=(LineSelection&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    // One compact record per selected logical line. The first cluster of line i
    // is zero for i == 0 and lines[i - 1].cluster_limit otherwise.
    std::pmr::vector<SelectedLineRecord> lines;
};

struct LineSelectionRequest final {
    const MultiRunShapedText* shaped_text{nullptr};
    const GlyphClusterMap* cluster_map{nullptr};
    const CaretBoundaryMap* caret_map{nullptr};
    const LineBreakOpportunityMap* opportunity_map{nullptr};
    std::uint32_t cluster_count{0};
    std::uint64_t available_inline_advance{0};
};

enum class LineSelectionErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    InconsistentTopology,
    UnsupportedDirection,
    InvalidGlyphSpan,
    AdvanceOverflow,
    OutputBudgetExceeded,
    AggregateOverflow
};

struct LineSelectionError final {
    LineSelectionErrorKind kind{LineSelectionErrorKind::None};
    std::size_t segment_index{0};
    std::size_t glyph_index{0};
    std::uint32_t cluster_index{0};
    std::uint32_t boundary_index{0};
    std::string message;
};

struct LineSelectionStats final {
    std::uint64_t input_segments{0};
    std::uint64_t input_glyphs{0};
    std::uint64_t input_clusters{0};
    std::uint64_t input_boundaries{0};
    std::uint64_t legal_boundaries{0};
    std::uint64_t suppressed_unsafe_boundaries{0};
    std::uint64_t zero_advance_clusters{0};
    std::uint64_t output_lines{0};
    std::uint64_t soft_break_lines{0};
    std::uint64_t mandatory_break_lines{0};
    std::uint64_t overflow_lines{0};
    std::uint64_t empty_lines{0};
    std::uint64_t total_inline_advance{0};
    std::uint64_t maximum_line_advance{0};
    std::uint64_t maximum_overflow_advance{0};
    std::uint64_t maximum_line_clusters{0};
};

const char* line_selection_error_kind_name(
    LineSelectionErrorKind kind) noexcept;

// Greedily selects the last legal, caret-safe boundary that fits the available
// inline advance. If no legal boundary fits, the next legal boundary is emitted
// as a controlled overflow line. Mandatory opportunities are never suppressed.
// Horizontal LTR and RTL runs are supported; vertical shaping is rejected.
// Output is empty after every failure.
bool select_bounded_lines(
    const LineSelectionRequest& request,
    LineSelection* output,
    LineSelectionStats* stats,
    LineSelectionError* error) noexcept;

std::uint32_t selected_line_first_cluster(
    const LineSelection& selection,
    std::size_t line_index) noexcept;

bool selected_line_has_flag(
    const LineSelection& selection,
    std::size_t line_index,
    SelectedLineFlags flag) noexcept;

} // namespace zevryon::text
