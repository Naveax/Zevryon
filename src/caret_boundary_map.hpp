#pragma once

#include "glyph_cluster_map.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <vector>

namespace zevryon::text {

enum CaretBoundaryFlags : std::uint8_t {
    kCaretBoundarySafe = 1U << 0U,
    kCaretBoundaryTextEdge = 1U << 1U,
    kCaretBoundaryRunEdge = 1U << 2U,
    kCaretBoundaryGlyphEdge = 1U << 3U,
    kCaretBoundaryInsideMergedGroup = 1U << 4U,
    kCaretBoundaryUnsafeToBreak = 1U << 5U
};

class CaretBoundaryMap final {
public:
    explicit CaretBoundaryMap(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    CaretBoundaryMap(const CaretBoundaryMap&) = delete;
    CaretBoundaryMap& operator=(const CaretBoundaryMap&) = delete;
    CaretBoundaryMap(CaretBoundaryMap&&) noexcept = default;
    CaretBoundaryMap& operator=(CaretBoundaryMap&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    // Exactly cluster_count + 1 bytes. Entry i describes the logical caret
    // boundary before cluster i; the final entry is the text-end sentinel.
    std::pmr::vector<std::uint8_t> flags;
};

struct CaretBoundaryMapRequest final {
    const MultiRunShapedText* shaped_text{nullptr};
    const GlyphClusterMap* cluster_map{nullptr};
    std::uint32_t cluster_count{0};
};

enum class CaretBoundaryMapErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    InconsistentClusterMap,
    InvalidGlyphSpan,
    OutputBudgetExceeded,
    AggregateOverflow
};

struct CaretBoundaryMapError final {
    CaretBoundaryMapErrorKind kind{CaretBoundaryMapErrorKind::None};
    std::size_t boundary_index{0};
    std::size_t segment_index{0};
    std::size_t glyph_index{0};
    std::string message;
};

struct CaretBoundaryMapStats final {
    std::uint64_t input_segments{0};
    std::uint64_t input_glyphs{0};
    std::uint64_t input_clusters{0};
    std::uint64_t output_boundaries{0};
    std::uint64_t glyph_groups{0};
    std::uint64_t safe_boundaries{0};
    std::uint64_t unsafe_boundaries{0};
    std::uint64_t text_edge_boundaries{0};
    std::uint64_t run_edge_boundaries{0};
    std::uint64_t glyph_edge_boundaries{0};
    std::uint64_t merged_interior_boundaries{0};
    std::uint64_t unsafe_to_break_boundaries{0};
};

const char* caret_boundary_map_error_kind_name(
    CaretBoundaryMapErrorKind kind) noexcept;

// Builds a compact, failure-atomic caret-safety index over the certified
// glyph-cluster map. Text edges are safe. Boundaries inside merged HarfBuzz
// groups are unsafe. Clean glyph-group edges are safe unless either adjacent
// group carries HarfBuzz unsafe-to-break evidence. No interior ligature
// coordinate is synthesized.
bool build_caret_boundary_map(
    const CaretBoundaryMapRequest& request,
    CaretBoundaryMap* output,
    CaretBoundaryMapStats* stats,
    CaretBoundaryMapError* error) noexcept;

bool caret_boundary_has_flag(
    const CaretBoundaryMap& map,
    std::uint32_t boundary_index,
    CaretBoundaryFlags flag) noexcept;

} // namespace zevryon::text
