#pragma once

#include "bidi_sequence.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

enum class BidiImplicitErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    TopologyViolation,
    OutputBudgetExceeded
};

const char* bidi_implicit_error_kind_name(BidiImplicitErrorKind kind) noexcept;

struct BidiImplicitError {
    BidiImplicitErrorKind kind{BidiImplicitErrorKind::None};
    std::size_t active_index{0};
    std::string message;
};

struct BidiImplicitStats {
    std::uint64_t input_units{0};
    std::uint64_t active_units{0};
    std::uint64_t isolating_sequences{0};
    std::uint64_t i1_r_changes{0};
    std::uint64_t i1_number_changes{0};
    std::uint64_t i2_l_changes{0};
    std::uint64_t i2_number_changes{0};
    std::uint64_t output_levels{0};
    std::uint8_t maximum_input_level{0};
    std::uint8_t maximum_output_level{0};
};

// Applies Unicode 17 UAX #9 I1-I2 after N0-N2. Input and output use
// BidiSequenceTopology::active_unit_indices order. The output stores exactly one
// resolved embedding-level byte per X9-active scalar. Explicit units, neutral
// types, and sequence topology remain immutable. On any validation or allocation
// failure, no partial output is published.
bool resolve_bidi_implicit_levels(
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    std::span<const BidiClass> neutral_types,
    std::pmr::vector<std::uint8_t>* resolved_levels,
    BidiImplicitStats* stats,
    BidiImplicitError* error) noexcept;

} // namespace zevryon::text
