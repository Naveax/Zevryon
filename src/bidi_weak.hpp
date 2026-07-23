#pragma once

#include "bidi_sequence.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

enum class BidiWeakErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    TopologyViolation,
    OutputBudgetExceeded
};

const char* bidi_weak_error_kind_name(BidiWeakErrorKind kind) noexcept;

struct BidiWeakError {
    BidiWeakErrorKind kind{BidiWeakErrorKind::None};
    std::size_t active_index{0};
    std::string message;
};

struct BidiWeakStats {
    std::uint64_t input_units{0};
    std::uint64_t active_units{0};
    std::uint64_t isolating_sequences{0};
    std::uint64_t w1_nsm_changes{0};
    std::uint64_t w2_en_to_an{0};
    std::uint64_t w3_al_to_r{0};
    std::uint64_t w4_separator_changes{0};
    std::uint64_t w5_et_to_en{0};
    std::uint64_t w6_neutralized{0};
    std::uint64_t w7_en_to_l{0};
    std::uint64_t output_types{0};
};

// Resolves UAX #9 W1-W7 independently for every isolating run sequence.
// Output index i corresponds to topology.active_unit_indices[i]. Explicit units
// and sequence topology remain immutable. No source text or sequence index list
// is copied.
bool resolve_bidi_weak_types(
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    std::pmr::vector<BidiClass>* resolved_types,
    BidiWeakStats* stats,
    BidiWeakError* error) noexcept;

} // namespace zevryon::text
