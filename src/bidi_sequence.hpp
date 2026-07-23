#pragma once

#include "bidi_explicit.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

enum class BidiSequenceErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    IndexOverflow,
    TopologyViolation,
    OutputBudgetExceeded
};

const char* bidi_sequence_error_kind_name(BidiSequenceErrorKind kind) noexcept;

struct BidiSequenceError {
    BidiSequenceErrorKind kind{BidiSequenceErrorKind::None};
    std::size_t unit_index{0};
    std::string message;
};

struct BidiLevelRun {
    std::uint32_t first_active_index{0};
    std::uint32_t active_count{0};
    std::uint8_t level{0};
    std::uint8_t reserved0{0};
    std::uint16_t reserved1{0};

    bool operator==(const BidiLevelRun&) const noexcept = default;
};

static_assert(
    sizeof(BidiLevelRun) <= 12U,
    "bidi level-run descriptors must remain within the Z1 memory contract");

struct BidiIsolatingRunSequence {
    std::uint32_t first_run_link{0};
    std::uint32_t run_count{0};
    BidiClass sos{BidiClass::L};
    BidiClass eos{BidiClass::L};
    std::uint8_t level{0};
    std::uint8_t reserved0{0};
    std::uint16_t reserved1{0};

    bool operator==(const BidiIsolatingRunSequence&) const noexcept = default;
};

static_assert(
    sizeof(BidiIsolatingRunSequence) <= 16U,
    "isolating-run-sequence descriptors must remain within the Z1 memory contract");

class BidiSequenceTopology {
public:
    explicit BidiSequenceTopology(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    BidiSequenceTopology(const BidiSequenceTopology&) = delete;
    BidiSequenceTopology& operator=(const BidiSequenceTopology&) = delete;
    BidiSequenceTopology(BidiSequenceTopology&&) noexcept = default;
    BidiSequenceTopology& operator=(BidiSequenceTopology&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    // Maps filtered X9 positions to BidiExplicitUnit indices.
    std::pmr::vector<std::uint32_t> active_unit_indices;
    // Level runs address active_unit_indices, not the original unit array.
    std::pmr::vector<BidiLevelRun> level_runs;
    // Each sequence references a contiguous slice of level-run IDs here.
    std::pmr::vector<std::uint32_t> sequence_run_indices;
    std::pmr::vector<BidiIsolatingRunSequence> sequences;
};

struct BidiSequenceStats {
    std::uint64_t input_units{0};
    std::uint64_t active_units{0};
    std::uint64_t removed_units{0};
    std::uint64_t level_runs{0};
    std::uint64_t isolating_sequences{0};
    std::uint64_t sequence_run_links{0};
    std::uint64_t matched_isolates{0};
    std::uint64_t unmatched_isolate_initiators{0};
    std::uint64_t unmatched_pdi{0};
    std::uint64_t maximum_sequence_runs{0};
    std::uint64_t maximum_sequence_units{0};
};

// Applies X9 virtually and constructs BD13/X10 isolating run sequences.
// No BidiExplicitUnit is copied. sos/eos are computed from the original
// explicit levels before any W/N/I mutation.
bool build_bidi_isolating_run_sequences(
    std::span<const BidiExplicitUnit> units,
    std::uint8_t paragraph_level,
    BidiSequenceTopology* topology,
    BidiSequenceStats* stats,
    BidiSequenceError* error) noexcept;

} // namespace zevryon::text
