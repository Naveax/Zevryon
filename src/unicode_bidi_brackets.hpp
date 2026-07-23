#pragma once

#include "unicode_bidi_brackets.generated.hpp"

#include <cstdint>

namespace zevryon::text {

struct BidiBracketInfo {
    std::uint32_t paired_codepoint{0};
    BidiBracketType type{BidiBracketType::None};
};

BidiBracketInfo bidi_bracket_info(std::uint32_t codepoint) noexcept;
bool bidi_brackets_match(
    std::uint32_t expected_closing,
    std::uint32_t actual_closing) noexcept;

} // namespace zevryon::text
