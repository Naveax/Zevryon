#pragma once

#include "font_line_metrics.hpp"
#include "line_fragment_layout.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <vector>

namespace zevryon::text {

enum LineBoxFlags : std::uint32_t {
    kLineBoxExpandedBeyondStrut = 1U << 19U,
    kLineBoxContainsMixedMetrics = 1U << 20U,
    kLineBoxContainsNegativeLineGap = 1U << 21U
};

enum FragmentBlockMetricFlags : std::uint32_t {
    kFragmentBlockMetricOs2Typographic = 1U << 0U,
    kFragmentBlockMetricHhea = 1U << 1U,
    kFragmentBlockMetricNegativeLineGap = 1U << 2U,
    kFragmentBlockMetricMatchesStrut = 1U << 3U
};

struct LineBoxRecord final {
    std::uint64_t block_start{0};
    std::uint64_t block_size{0};
    std::uint64_t baseline{0};
    std::uint64_t inline_advance{0};
    std::uint32_t first_fragment_metric{0};
    std::uint32_t fragment_metric_count{0};
    std::uint32_t cluster_limit{0};
    std::uint32_t flags{0};

    bool operator==(const LineBoxRecord&) const noexcept = default;
};

static_assert(
    sizeof(LineBoxRecord) == 48U,
    "line-box records must remain within the Z2 memory contract");

struct FragmentBlockMetric final {
    std::uint64_t block_offset{0};
    std::uint64_t block_size{0};
    std::uint64_t baseline_offset{0};
    std::uint32_t flags{0};
    std::uint32_t reserved{0};

    bool operator==(const FragmentBlockMetric&) const noexcept = default;
};

static_assert(
    sizeof(FragmentBlockMetric) == 32U,
    "fragment block metrics must remain within the Z2 memory contract");

class LineBoxLayout final {
public:
    explicit LineBoxLayout(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    LineBoxLayout(const LineBoxLayout&) = delete;
    LineBoxLayout& operator=(const LineBoxLayout&) = delete;
    LineBoxLayout(LineBoxLayout&&) noexcept = default;
    LineBoxLayout& operator=(LineBoxLayout&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    // Lines remain in logical block order. fragment_metrics is parallel to the
    // source visual fragment array and each line references the same slice.
    std::pmr::vector<LineBoxRecord> lines;
    std::pmr::vector<FragmentBlockMetric> fragment_metrics;
};

struct LineBoxLayoutRequest final {
    const LineFragmentLayout* fragment_layout{nullptr};
    const MultiRunShapedText* shaped_text{nullptr};
    const FontLineMetricTable* font_metrics{nullptr};
    FontFaceId strut_face_id{kInvalidFontFaceId};
    std::int32_t strut_y_scale{0};
};

enum class LineBoxLayoutErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    InvalidMetricTable,
    TopologyViolation,
    MissingFaceMetrics,
    InvalidScale,
    MetricOverflow,
    OutputBudgetExceeded,
    AggregateOverflow
};

struct LineBoxLayoutError final {
    LineBoxLayoutErrorKind kind{LineBoxLayoutErrorKind::None};
    std::size_t line_index{0};
    std::size_t fragment_index{0};
    std::uint32_t segment_index{0};
    FontFaceId face_id{kInvalidFontFaceId};
    std::string message;
};

struct LineBoxLayoutStats final {
    std::uint64_t input_lines{0};
    std::uint64_t input_fragments{0};
    std::uint64_t input_segments{0};
    std::uint64_t input_metric_records{0};
    std::uint64_t output_lines{0};
    std::uint64_t output_fragment_metrics{0};
    std::uint64_t empty_lines{0};
    std::uint64_t expanded_lines{0};
    std::uint64_t mixed_metric_lines{0};
    std::uint64_t os2_fragment_metrics{0};
    std::uint64_t hhea_fragment_metrics{0};
    std::uint64_t negative_gap_fragment_metrics{0};
    std::uint64_t total_block_extent{0};
    std::uint64_t maximum_line_block_size{0};
    std::uint64_t maximum_line_ascent{0};
    std::uint64_t maximum_line_descent{0};
    std::uint64_t maximum_fragment_block_size{0};
};

const char* line_box_layout_error_kind_name(LineBoxLayoutErrorKind kind) noexcept;

// Resolves normal horizontal line boxes from visual fragments. Every line starts
// with one strut metric; fragment metrics can expand but never shrink it. Font
// design metrics are scaled with the exact y_scale retained by HarfBuzz. Output
// is empty after every failure.
bool build_line_box_layout(
    const LineBoxLayoutRequest& request,
    LineBoxLayout* output,
    LineBoxLayoutStats* stats,
    LineBoxLayoutError* error) noexcept;

} // namespace zevryon::text
