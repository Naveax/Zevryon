#pragma once

#include <cstdint>

namespace zevryon::text {

enum class GraphemeBreakClass : std::uint8_t {
    Other = 0,
    CR,
    LF,
    Control,
    Extend,
    ZWJ,
    RegionalIndicator,
    Prepend,
    SpacingMark,
    L,
    V,
    T,
    LV,
    LVT
};

enum class IndicConjunctBreak : std::uint8_t {
    None = 0,
    Consonant,
    Extend,
    Linker
};

struct GraphemeProperties {
    GraphemeBreakClass break_class{GraphemeBreakClass::Other};
    IndicConjunctBreak indic_conjunct_break{IndicConjunctBreak::None};
    bool extended_pictographic{false};
};

constexpr const char* kUnicodeGraphemeDataVersion = "17.0.0";
constexpr const char* kUnicodeGraphemeGenerator = "regex-2026.5.9";
constexpr const char* kUnicodeGraphemeDataFingerprint =
    "fde26abeb242fbe7c5a3d5ced5d4666286a08baaf15822f71e7a0af9f9ff5a91";

GraphemeProperties grapheme_properties(std::uint32_t code_point) noexcept;

} // namespace zevryon::text
