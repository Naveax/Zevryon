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

struct GraphemeCluster {
    std::uint64_t source_start{0};
    std::uint32_t source_length{0};
    std::uint32_t first_codepoint{0};
    std::uint32_t codepoint_count{0};

    constexpr std::uint64_t source_end() const noexcept {
        return source_start + static_cast<std::uint64_t>(source_length);
    }

    bool operator==(const GraphemeCluster&) const noexcept = default;
};

static_assert(
    sizeof(GraphemeCluster) <= 24U,
    "grapheme cluster records must remain within the Z1 memory contract");

struct GraphemeSegmentStats {
    std::uint64_t input_codepoints{0};
    std::uint64_t output_clusters{0};
    std::uint64_t suppressed_breaks{0};
    std::uint64_t maximum_cluster_codepoints{0};
    std::uint64_t maximum_cluster_source_bytes{0};
};

bool segment_graphemes(
    std::span<const DecodedCodePoint> codepoints,
    std::pmr::vector<GraphemeCluster>* clusters,
    GraphemeSegmentStats* stats,
    GraphemeError* error) noexcept;

} // namespace zevryon::text
