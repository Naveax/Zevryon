#pragma once

#include "unicode_bidi_data.generated.hpp"

#include <cstdint>
#include <string_view>

namespace zevryon::text {

struct BidiBracketInfo {
    std::uint32_t paired_codepoint{0};
    BidiBracketType type{BidiBracketType::None};
};

BidiClass bidi_class_of(std::uint32_t codepoint) noexcept;
BidiBracketInfo bidi_bracket_of(std::uint32_t codepoint) noexcept;
bool bidi_mirror_of(std::uint32_t codepoint, std::uint32_t* mirror) noexcept;
std::string_view bidi_class_short_name(BidiClass value) noexcept;
std::string_view bidi_class_long_name(BidiClass value) noexcept;
bool bidi_class_from_name(std::string_view name, BidiClass* value) noexcept;

constexpr bool is_isolate_initiator(BidiClass value) noexcept {
    return value == BidiClass::LRI ||
           value == BidiClass::RLI ||
           value == BidiClass::FSI;
}

constexpr bool is_embedding_initiator(BidiClass value) noexcept {
    return value == BidiClass::LRE || value == BidiClass::RLE ||
           value == BidiClass::LRO || value == BidiClass::RLO;
}

constexpr bool is_removed_by_x9(BidiClass value) noexcept {
    return is_embedding_initiator(value) ||
           value == BidiClass::PDF || value == BidiClass::BN;
}

} // namespace zevryon::text
