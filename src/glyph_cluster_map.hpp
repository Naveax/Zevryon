#pragma once

#include "multi_run_harfbuzz_shaper.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <vector>

namespace zevryon::text {

struct GlyphClusterRecord {
    std::uint32_t segment_index{0};
    std::uint32_t owner_cluster{0};
    std::uint32_t first_glyph{0};
    std::uint32_t glyph_count{0};

    bool operator==(const GlyphClusterRecord&) const noexcept = default;
};

static_assert(
    sizeof(GlyphClusterRecord) == 16U,
    "glyph-cluster records must remain within the Z2 memory contract");

enum class GlyphClusterMapErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    SegmentTopologyViolation,
    InvalidGlyphCluster,
    NonMonotoneGlyphClusters,
    OutputBudgetExceeded
};

struct GlyphClusterMapError {
    GlyphClusterMapErrorKind kind{GlyphClusterMapErrorKind::None};
    std::size_t segment_index{0};
    std::size_t glyph_index{0};
    std::uint32_t cluster_index{0};
    std::string message;
};

struct GlyphClusterMapStats {
    std::uint64_t input_segments{0};
    std::uint64_t input_glyphs{0};
    std::uint64_t input_clusters{0};
    std::uint64_t output_records{0};
    std::uint64_t owner_clusters{0};
    std::uint64_t continuation_clusters{0};
    std::uint64_t left_to_right_segments{0};
    std::uint64_t right_to_left_segments{0};
    std::uint64_t maximum_group_glyphs{0};
    std::uint64_t maximum_owner_span_clusters{0};
};

class GlyphClusterMap final {
public:
    explicit GlyphClusterMap(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    GlyphClusterMap(const GlyphClusterMap&) = delete;
    GlyphClusterMap& operator=(const GlyphClusterMap&) = delete;
    GlyphClusterMap(GlyphClusterMap&&) noexcept = default;
    GlyphClusterMap& operator=(GlyphClusterMap&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    // Exactly one logical record per grapheme cluster. Continuation clusters
    // created by ligatures or other HarfBuzz cluster merges reference the same
    // owner cluster and contiguous glyph group as their owner.
    std::pmr::vector<GlyphClusterRecord> records;
};

const char* glyph_cluster_map_error_kind_name(
    GlyphClusterMapErrorKind kind) noexcept;

// Builds an O(1) logical cluster lookup over segmented HarfBuzz output. Every
// segment must cover a contiguous logical cluster range and use HarfBuzz
// MONOTONE_GRAPHEMES output: non-decreasing cluster values for LTR and
// non-increasing values for RTL. Output is empty after every failure.
bool build_glyph_cluster_map(
    const MultiRunShapedText& shaped_text,
    std::uint32_t cluster_count,
    GlyphClusterMap* output,
    GlyphClusterMapStats* stats,
    GlyphClusterMapError* error) noexcept;

} // namespace zevryon::text
