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

enum class BidiParagraphDirection : std::uint8_t {
    Left = 0,
    Right = 1,
    Auto = 0xffU
};

enum class BidiOverrideStatus : std::uint8_t {
    Neutral = 0,
    Left,
    Right
};

enum class BidiExplicitErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    OutputBudgetExceeded
};

inline constexpr std::uint8_t kBidiUnitRemovedByX9 = 1U << 0U;
inline constexpr std::uint8_t kBidiUnitIsolateInitiator = 1U << 1U;
inline constexpr std::uint8_t kBidiUnitPopDirectionalIsolate = 1U << 2U;

struct BidiExplicitError {
    BidiExplicitErrorKind kind{BidiExplicitErrorKind::None};
    std::size_t codepoint_index{0};
    std::string message;
};

// Level is the explicit embedding level assigned by UAX #9 X1-X8. Retained
// embedding initiators use the surrounding pre-push level; PDF and PDI use the
// post-pop level. Units flagged RemovedByX9 are discarded by the next stage.
struct BidiExplicitUnit {
    std::uint64_t source_offset{0};
    std::uint32_t codepoint_index{0};
    BidiClass original_class{BidiClass::L};
    BidiClass resolved_class{BidiClass::L};
    std::uint8_t level{0};
    std::uint8_t flags{0};

    bool operator==(const BidiExplicitUnit&) const noexcept = default;
};

static_assert(
    sizeof(BidiExplicitUnit) <= 16U,
    "bidi explicit units must remain within the Z1 memory contract");

struct BidiExplicitStats {
    std::uint64_t input_codepoints{0};
    std::uint64_t output_units{0};
    std::uint64_t explicit_controls{0};
    std::uint64_t isolate_initiators{0};
    std::uint64_t fsi_resolutions{0};
    std::uint64_t valid_isolates{0};
    std::uint64_t overflow_isolates{0};
    std::uint64_t overflow_embeddings{0};
    std::uint64_t unmatched_pdi{0};
    std::uint64_t unmatched_pdf{0};
    std::uint8_t paragraph_level{0};
    std::uint8_t maximum_level{0};
};

const char* bidi_explicit_error_kind_name(BidiExplicitErrorKind kind) noexcept;

// The input is exactly one paragraph. A paragraph separator, when present, must
// be the final scalar and is assigned the paragraph embedding level.
bool resolve_bidi_explicit(
    std::span<const DecodedCodePoint> codepoints,
    BidiParagraphDirection direction,
    std::pmr::vector<BidiExplicitUnit>* units,
    BidiExplicitStats* stats,
    BidiExplicitError* error) noexcept;

} // namespace zevryon::text
