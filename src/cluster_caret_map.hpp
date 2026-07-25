#pragma once

#include "multi_run_harfbuzz_shaper.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <string>
#include <vector>

namespace zevryon::text {

constexpr std::uint32_t kInvalidClusterMapIndex =
    std::numeric_limits<std::uint32_t>::max();

enum ClusterGlyphMapFlags : std::uint32_t {
    kClusterGlyphMapRightToLeft = 1U << 0U,
    kClusterGlyphMapMergedGroup = 1U << 1U,
    kClusterGlyphMapUnsafeToBreak = 1U << 2U,
    kClusterGlyphMapUnsafeToConcat = 1U << 3U,
    kClusterGlyphMapSafeToInsertTatweel = 1U << 4U,
    kClusterGlyphMapMissingGlyph = 1U << 5U
};

// One compact record per logical grapheme cluster. Multiple clusters consumed by
// one ligature or another merged HarfBuzz cluster group intentionally carry the
// same segment-local glyph span and logical group range.
struct ClusterGlyphMapEntry final {
    std::uint32_t segment_index{kInvalidClusterMapIndex};
    std::uint32_t first_glyph{0};
    std::uint32_t glyph_count{0};
    std::uint32_t group_first_cluster{0};
    std::uint32_t group_cluster_limit{0};
    std::uint32_t flags{0};

    bool operator==(const ClusterGlyphMapEntry&) const noexcept = default;
};

static_assert(
    sizeof(ClusterGlyphMapEntry) == 24U,
    "cluster-to-glyph records must remain within the Z2 memory contract");

class ClusterCaretMap final {
public:
    explicit ClusterCaretMap(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    ClusterCaretMap(const ClusterCaretMap&) = delete;
    ClusterCaretMap& operator=(const ClusterCaretMap&) = delete;
    ClusterCaretMap(ClusterCaretMap&&) noexcept = default;
    ClusterCaretMap& operator=(ClusterCaretMap&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    std::pmr::vector<ClusterGlyphMapEntry> clusters;
};

struct ClusterCaretMapRequest final {
    const MultiRunShapedText* shaped_text{nullptr};
    std::uint32_t cluster_count{0};
};

enum class ClusterCaretMapErrorKind : std::uint8_t {
    None = 0,
    InvalidArgument,
    InvalidSegmentTable,
    InvalidGlyphCluster,
    InvalidGlyphOrder,
    ClusterCoverageFailure,
    MetadataBudgetExceeded,
    AggregateOverflow
};

struct ClusterCaretMapError final {
    ClusterCaretMapErrorKind kind{ClusterCaretMapErrorKind::None};
    std::size_t segment_index{0};
    std::size_t glyph_index{0};
    std::uint32_t cluster_index{0};
    std::string message;
};

enum CaretBoundaryFlags : std::uint32_t {
    kCaretBoundarySafe = 1U << 0U,
    kCaretBoundaryTextEdge = 1U << 1U,
    kCaretBoundaryRunEdge = 1U << 2U,
    kCaretBoundaryGlyphEdge = 1U << 3U,
    kCaretBoundaryInsideMergedGroup = 1U << 4U,
    kCaretBoundaryUnsafeToBreak = 1U << 5U
};

// A zero-allocation view of one logical grapheme boundary. This does not invent
// interior ligature coordinates: boundaries inside a merged glyph group are
// explicitly reported as unsafe and non-glyph-edge boundaries.
struct CaretBoundaryInfo final {
    std::uint32_t boundary_index{0};
    std::uint32_t left_cluster{kInvalidClusterMapIndex};
    std::uint32_t right_cluster{kInvalidClusterMapIndex};
    std::uint32_t flags{0};
};

struct ClusterCaretMapStats final {
    std::uint64_t input_segments{0};
    std::uint64_t input_glyphs{0};
    std::uint64_t input_clusters{0};
    std::uint64_t output_entries{0};
    std::uint64_t glyph_groups{0};
    std::uint64_t merged_groups{0};
    std::uint64_t clusters_in_merged_groups{0};
    std::uint64_t unsafe_groups{0};
    std::uint64_t missing_glyph_groups{0};
    std::uint64_t left_to_right_groups{0};
    std::uint64_t right_to_left_groups{0};
    std::uint64_t safe_caret_boundaries{0};
    std::uint64_t unsafe_caret_boundaries{0};
    std::size_t maximum_group_glyphs{0};
    std::uint32_t maximum_group_clusters{0};
};

const char* cluster_caret_map_error_kind_name(
    ClusterCaretMapErrorKind kind) noexcept;

// Builds a failure-atomic logical cluster-to-glyph map from the retained Z2C-2
// segments. Glyph storage is referenced by segment-local indices and is never
// flattened or copied. The map is empty after every failure.
bool build_cluster_caret_map(
    const ClusterCaretMapRequest& request,
    ClusterCaretMap* output,
    ClusterCaretMapStats* stats,
    ClusterCaretMapError* error) noexcept;

// Inspects one boundary in O(1) with no allocation. Text edges are always safe;
// interior boundaries are safe only when they coincide with a glyph-group edge
// and neither adjacent group carries HarfBuzz unsafe-to-break evidence.
bool inspect_caret_boundary(
    const ClusterCaretMap& map,
    std::uint32_t boundary_index,
    CaretBoundaryInfo* output) noexcept;

} // namespace zevryon::text
