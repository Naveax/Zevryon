#pragma once

#include "unicode_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

enum class GraphemeErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    SourceRangeOverflow,
    OutputBudgetExceeded
};

const char* grapheme_error_kind_name(GraphemeErrorKind kind) noexcept;

struct GraphemeError {
    GraphemeErrorKind kind{GraphemeErrorKind::None};
    std::size_t codepoint_index{0};
    std::string message;
};

struct GraphemeBoundary {
    std::uint64_t source_offset{0};
    std::uint32_t codepoint_index{0};

    bool operator==(const GraphemeBoundary&) const noexcept = default;
};

static_assert(
    sizeof(GraphemeBoundary) <= 16U,
    "grapheme boundary records must remain within the Z1 memory contract");

struct GraphemeSegmentStats {
    std::uint64_t input_codepoints{0};
    std::uint64_t output_clusters{0};
    std::uint64_t suppressed_breaks{0};
    std::uint64_t maximum_cluster_codepoints{0};
    std::uint64_t maximum_cluster_source_bytes{0};
};

// For non-empty input, output contains one boundary for every cluster start plus
// one final sentinel boundary at the end of the input. Cluster i spans
// boundaries[i]..boundaries[i + 1]. Empty input produces an empty boundary list.
bool segment_graphemes(
    std::span<const DecodedCodePoint> codepoints,
    std::pmr::vector<GraphemeBoundary>* boundaries,
    GraphemeSegmentStats* stats,
    GraphemeError* error) noexcept;

} // namespace zevryon::text
