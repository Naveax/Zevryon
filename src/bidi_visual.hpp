#pragma once

#include "bidi_sequence.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

struct BidiLineSpan {
    std::uint32_t first_active_index{0};
    std::uint32_t active_count{0};

    bool operator==(const BidiLineSpan&) const noexcept = default;
};

static_assert(
    sizeof(BidiLineSpan) <= 8U,
    "bidi line spans must remain within the Z1 memory contract");

enum class BidiVisualErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    TopologyViolation,
    LinePartitionViolation,
    OutputBudgetExceeded
};

const char* bidi_visual_error_kind_name(BidiVisualErrorKind kind) noexcept;

struct BidiVisualError {
    BidiVisualErrorKind kind{BidiVisualErrorKind::None};
    std::size_t active_index{0};
    std::string message;
};

struct BidiVisualStats {
    std::uint64_t input_units{0};
    std::uint64_t active_units{0};
    std::uint64_t lines{0};
    std::uint64_t l1_separator_resets{0};
    std::uint64_t l1_whitespace_resets{0};
    std::uint64_t l1_isolate_resets{0};
    std::uint64_t l2_reversal_spans{0};
    std::uint64_t l2_reversed_units{0};
    std::uint64_t l3_combining_sequences{0};
    std::uint64_t l3_repaired_units{0};
    std::uint64_t output_levels{0};
    std::uint64_t output_visual_indices{0};
    std::uint8_t maximum_input_level{0};
    std::uint8_t maximum_line_level{0};
};

class BidiVisualOrder {
public:
    explicit BidiVisualOrder(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    BidiVisualOrder(const BidiVisualOrder&) = delete;
    BidiVisualOrder& operator=(const BidiVisualOrder&) = delete;
    BidiVisualOrder(BidiVisualOrder&&) noexcept = default;
    BidiVisualOrder& operator=(BidiVisualOrder&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    // L1-adjusted level for each X9-active scalar.
    std::pmr::vector<std::uint8_t> line_levels;
    // Concatenated per-line visual order. Each value is an active index into
    // BidiSequenceTopology::active_unit_indices.
    std::pmr::vector<std::uint32_t> visual_to_active;
};

// Applies UAX #9 L1-L3 to caller-supplied line partitions. Lines must form an
// exact, contiguous partition of the X9-active stream. The input implicit levels,
// explicit units, and sequence topology remain immutable. Output vectors are
// published together only after validation and all line reordering succeeds.
bool resolve_bidi_visual_order(
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    std::span<const std::uint8_t> implicit_levels,
    std::uint8_t paragraph_level,
    std::span<const BidiLineSpan> lines,
    BidiVisualOrder* output,
    BidiVisualStats* stats,
    BidiVisualError* error) noexcept;

} // namespace zevryon::text
