#pragma once

#include <cstdint>

namespace zevryon::text {

enum class LineBreakClass : std::uint8_t {
    AL = 0,
    BK,
    CR,
    LF,
    NL,
    SP,
    ZW,
    ZWJ,
    CM,
    WJ,
    GL,
    B2,
    BA,
    BB,
    HH,
    HY,
    CB,
    CL,
    CP,
    EX,
    IN,
    NS,
    OP,
    QU,
    IS,
    NU,
    PO,
    PR,
    SY,
    H2,
    H3,
    HL,
    ID,
    JL,
    JV,
    JT,
    RI,
    EB,
    EM,
    AK,
    AP,
    AS,
    VF,
    VI
};

enum LineBreakPropertyFlags : std::uint8_t {
    kLineBreakInitialQuote = 1U << 0U,
    kLineBreakFinalQuote = 1U << 1U,
    kLineBreakEastAsian = 1U << 2U,
    kLineBreakExtendedPictographicUnassigned = 1U << 3U,
    kLineBreakDottedCircle = 1U << 4U
};

struct LineBreakProperties final {
    LineBreakClass break_class{LineBreakClass::AL};
    std::uint8_t flags{0};

    bool operator==(const LineBreakProperties&) const noexcept = default;
};

constexpr const char* kUnicodeLineBreakDataVersion = "17.0.0";
constexpr const char* kUnicodeLineBreakGenerator = "regex-2026.5.9";
constexpr const char* kUnicodeLineBreakDataFingerprint =
    "3f256b5af69b5e7cfd5e9dddfbbe3b28cb18c98746dca47753b102fa3e99a4e4";

LineBreakProperties line_break_properties(std::uint32_t code_point) noexcept;
const char* line_break_class_name(LineBreakClass value) noexcept;

} // namespace zevryon::text
