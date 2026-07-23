#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace zevryon::text {

inline constexpr std::string_view kUnicodeBidiDataVersion = "17.0.0";
inline constexpr std::string_view kUnicodeBidiDataFingerprint = "bootstrap-placeholder";
inline constexpr std::string_view kDerivedBidiClasstxtSha256 = "bootstrap-placeholder";
inline constexpr std::string_view kPropertyValueAliasestxtSha256 = "bootstrap-placeholder";

enum class BidiClass : std::uint8_t {
    L = 0,
    R,
    AL,
    EN,
    ES,
    ET,
    AN,
    CS,
    NSM,
    BN,
    B,
    S,
    WS,
    ON,
    LRE,
    LRO,
    RLE,
    RLO,
    PDF,
    LRI,
    RLI,
    FSI,
    PDI,
    Count
};

struct BidiClassRange {
    std::uint32_t start;
    std::uint32_t end;
    BidiClass value;
};

inline constexpr std::array<std::string_view, 23> kBidiClassShortNames{{
    "L", "R", "AL", "EN", "ES", "ET", "AN", "CS", "NSM", "BN",
    "B", "S", "WS", "ON", "LRE", "LRO", "RLE", "RLO", "PDF",
    "LRI", "RLI", "FSI", "PDI",
}};
inline constexpr std::array<std::string_view, 23> kBidiClassLongNames{{
    "Left_To_Right", "Right_To_Left", "Arabic_Letter", "European_Number",
    "European_Separator", "European_Terminator", "Arabic_Number",
    "Common_Separator", "Nonspacing_Mark", "Boundary_Neutral",
    "Paragraph_Separator", "Segment_Separator", "White_Space", "Other_Neutral",
    "Left_To_Right_Embedding", "Left_To_Right_Override", "Right_To_Left_Embedding",
    "Right_To_Left_Override", "Pop_Directional_Format", "Left_To_Right_Isolate",
    "Right_To_Left_Isolate", "First_Strong_Isolate", "Pop_Directional_Isolate",
}};
inline constexpr std::array<BidiClassRange, 1> kBidiClassRanges{{
    BidiClassRange{0x0U, 0x10FFFFU, BidiClass::L},
}};

static_assert(kBidiClassShortNames.size() == static_cast<std::size_t>(BidiClass::Count));
static_assert(kBidiClassLongNames.size() == static_cast<std::size_t>(BidiClass::Count));

} // namespace zevryon::text
