#pragma once

#include "unicode_bidi_mirroring.generated.hpp"

#include <cstdint>

namespace zevryon::text {

struct BidiMirroringInfo {
    bool mirrored{false};
    bool has_character_mapping{false};
    bool best_fit{false};
    std::uint32_t mirror_codepoint{0};
};

BidiMirroringInfo bidi_mirroring_info(std::uint32_t codepoint) noexcept;

} // namespace zevryon::text
