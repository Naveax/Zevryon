#pragma once

#include "grapheme_segmenter.hpp"
#include "unicode_line_break_data.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

enum class LineBreakOpportunity : std::uint8_t {
    Prohibited = 0,
    Allowed = 1,
    Mandatory = 2
};

class LineBreakOpportunityMap final {
public:
    explicit LineBreakOpportunityMap(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    LineBreakOpportunityMap(const LineBreakOpportunityMap&) = delete;
    LineBreakOpportunityMap& operator=(const LineBreakOpportunityMap&) = delete;
    LineBreakOpportunityMap(LineBreakOpportunityMap&&) noexcept = default;
    LineBreakOpportunityMap& operator=(LineBreakOpportunityMap&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    // Exactly cluster_count + 1 bytes. Entry i describes the logical boundary
    // before grapheme cluster i; the final entry is the mandatory end boundary.
    std::pmr::vector<std::uint8_t> opportunities;
};

struct LineBreakOpportunityRequest final {
    std::span<const DecodedCodePoint> codepoints;
    std::span<const GraphemeBoundary> grapheme_boundaries;
};

enum class LineBreakOpportunityErrorKind : std::uint8_t {
    None = 0,
    InvalidCodePointStream,
    InvalidGraphemeTopology,
    ClusterDomainOverflow,
    WorkingMemoryBudgetExceeded,
    AggregateOverflow
};

struct LineBreakOpportunityError final {
    LineBreakOpportunityErrorKind kind{LineBreakOpportunityErrorKind::None};
    std::size_t codepoint_index{0};
    std::size_t cluster_index{0};
    std::string message;
};

struct LineBreakOpportunityStats final {
    std::uint64_t input_codepoints{0};
    std::uint64_t input_clusters{0};
    std::uint64_t output_boundaries{0};
    std::uint64_t significant_clusters{0};
    std::uint64_t ignored_combining_clusters{0};
    std::uint64_t mandatory_boundaries{0};
    std::uint64_t allowed_boundaries{0};
    std::uint64_t prohibited_boundaries{0};
    std::uint64_t hard_break_clusters{0};
    std::uint64_t maximum_cluster_codepoints{0};
    std::uint64_t maximum_cluster_source_bytes{0};
};

const char* line_break_opportunity_error_kind_name(
    LineBreakOpportunityErrorKind kind) noexcept;

// Implements the Unicode 17 UAX #14 default rules with the documented
// grapheme-cluster tailoring: mandatory rules are preserved, then every
// grapheme cluster is represented by its first code point. The result only
// identifies legal opportunities; width-constrained line selection is a
// separate layout stage.
bool build_line_break_opportunity_map(
    const LineBreakOpportunityRequest& request,
    LineBreakOpportunityMap* output,
    LineBreakOpportunityStats* stats,
    LineBreakOpportunityError* error) noexcept;

LineBreakOpportunity line_break_opportunity_at(
    const LineBreakOpportunityMap& map,
    std::uint32_t boundary_index) noexcept;

} // namespace zevryon::text
