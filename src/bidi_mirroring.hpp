#pragma once

#include "bidi_visual.hpp"
#include "unicode_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

enum class BidiMirrorKind : std::uint8_t {
    ExactCharacter = 0,
    BestFitCharacter,
    MirroredGlyphOnly
};

struct BidiMirrorRequest {
    std::uint32_t visual_index{0};
    std::uint32_t mirror_codepoint{0};
    BidiMirrorKind kind{BidiMirrorKind::MirroredGlyphOnly};
    std::uint8_t reserved0{0};
    std::uint16_t reserved1{0};

    bool operator==(const BidiMirrorRequest&) const noexcept = default;
};

static_assert(
    sizeof(BidiMirrorRequest) == 12U,
    "bidi mirror requests must remain exactly 12 bytes");

enum class BidiMirrorErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    OutputBudgetExceeded
};

const char* bidi_mirror_error_kind_name(BidiMirrorErrorKind kind) noexcept;

struct BidiMirrorError {
    BidiMirrorErrorKind kind{BidiMirrorErrorKind::None};
    std::size_t visual_index{0};
    std::string message;
};

struct BidiMirrorStats {
    std::uint64_t input_codepoints{0};
    std::uint64_t active_units{0};
    std::uint64_t visual_units{0};
    std::uint64_t odd_level_units{0};
    std::uint64_t mirrored_property_hits{0};
    std::uint64_t exact_character_requests{0};
    std::uint64_t best_fit_character_requests{0};
    std::uint64_t glyph_only_requests{0};
    std::uint64_t output_requests{0};
};

class BidiMirrorRequests {
public:
    explicit BidiMirrorRequests(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    BidiMirrorRequests(const BidiMirrorRequests&) = delete;
    BidiMirrorRequests& operator=(const BidiMirrorRequests&) = delete;
    BidiMirrorRequests(BidiMirrorRequests&&) noexcept = default;
    BidiMirrorRequests& operator=(BidiMirrorRequests&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    // Sparse requests sorted by visual_index. The source stream is never
    // rewritten. mirror_codepoint is zero for MirroredGlyphOnly requests.
    std::pmr::vector<BidiMirrorRequest> requests;
};

// Applies UAX #9 L4 after L1-L3. The C2A visual order is a trusted stage
// contract: visual_to_active must be a permutation of the active stream.
// Only odd-level scalars with normative Bidi_Mirrored=Y produce output.
bool build_bidi_mirror_requests(
    std::span<const DecodedCodePoint> codepoints,
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    const BidiVisualOrder& visual,
    BidiMirrorRequests* output,
    BidiMirrorStats* stats,
    BidiMirrorError* error) noexcept;

} // namespace zevryon::text
