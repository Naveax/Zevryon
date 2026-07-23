#pragma once

#include "bidi_sequence.hpp"
#include "unicode_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

enum class BidiNeutralErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    TopologyViolation,
    OutputBudgetExceeded
};

const char* bidi_neutral_error_kind_name(BidiNeutralErrorKind kind) noexcept;

struct BidiNeutralError {
    BidiNeutralErrorKind kind{BidiNeutralErrorKind::None};
    std::size_t active_index{0};
    std::string message;
};

struct BidiNeutralStats {
    std::uint64_t input_units{0};
    std::uint64_t active_units{0};
    std::uint64_t isolating_sequences{0};
    std::uint64_t bracket_candidates{0};
    std::uint64_t bracket_pairs{0};
    std::uint64_t bracket_overflow_sequences{0};
    std::uint64_t n0_embedding_pairs{0};
    std::uint64_t n0_opposite_pairs{0};
    std::uint64_t n0_following_nsm_changes{0};
    std::uint64_t n1_changes{0};
    std::uint64_t n2_changes{0};
    std::uint64_t output_types{0};
};

// Applies UAX #9 N0-N2 independently to each isolating run sequence. Input
// weak_types uses active-index order. Output uses the same order and stores one
// BidiClass byte per active scalar. Explicit units, weak types, and topology are
// immutable. Paired-bracket search uses a fixed 63-entry stack and fails closed
// for a sequence when BD16 overflows.
bool resolve_bidi_neutral_types(
    std::span<const DecodedCodePoint> codepoints,
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    std::span<const BidiClass> weak_types,
    std::pmr::vector<BidiClass>* resolved_types,
    BidiNeutralStats* stats,
    BidiNeutralError* error) noexcept;

} // namespace zevryon::text
