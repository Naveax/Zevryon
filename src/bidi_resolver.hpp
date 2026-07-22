#pragma once

#include "unicode_bidi.hpp"
#include "unicode_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

inline constexpr std::uint8_t kBidiRemovedLevel = 0xffU;

enum class ParagraphDirection : std::uint8_t {
    LeftToRight = 0,
    RightToLeft = 1,
    Auto = 2
};

enum class BidiErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    IndexOverflow,
    OutputBudgetExceeded,
    InternalInvariant
};

const char* bidi_error_kind_name(BidiErrorKind kind) noexcept;

struct BidiError {
    BidiErrorKind kind{BidiErrorKind::None};
    std::size_t input_index{0};
    std::string message;
};

struct BidiStats {
    std::uint64_t input_units{0};
    std::uint64_t removed_units{0};
    std::uint64_t level_runs{0};
    std::uint64_t isolating_run_sequences{0};
    std::uint64_t bracket_pairs{0};
    std::uint64_t bracket_stack_overflows{0};
    std::uint64_t isolate_overflows{0};
    std::uint64_t embedding_overflows{0};
    std::uint64_t override_changes{0};
    std::uint64_t maximum_sequence_units{0};
    std::uint8_t paragraph_level{0};
    std::uint8_t maximum_resolved_level{0};
};

// Resolve a synthetic sequence of Bidi_Class values. This form is used by the
// normative BidiTest corpus and deliberately disables paired-bracket lookup.
bool resolve_bidi_classes(
    std::span<const BidiClass> classes,
    ParagraphDirection direction,
    std::pmr::vector<std::uint8_t>* resolved_levels,
    std::pmr::vector<std::uint32_t>* visual_order,
    BidiStats* stats,
    BidiError* error) noexcept;

// Resolve a contiguous decoded scalar sequence using Unicode 17 properties,
// paired brackets, and mirroring data. `resolved_levels` contains one entry per
// input codepoint; X9-removed formatting characters use kBidiRemovedLevel.
// `visual_order` contains only non-X9-removed logical indices in L2 order.
bool resolve_bidi_codepoints(
    std::span<const DecodedCodePoint> codepoints,
    ParagraphDirection direction,
    std::pmr::vector<std::uint8_t>* resolved_levels,
    std::pmr::vector<std::uint32_t>* visual_order,
    BidiStats* stats,
    BidiError* error) noexcept;

} // namespace zevryon::text
