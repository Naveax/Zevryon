#include "unicode_line_break_data.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace zevryon::text {
namespace {

constexpr std::array<std::uint32_t, 3635U> kRangeEnds{{
#include "unicode_line_break_ends.inc"
}};

// Low byte: LineBreakClass. High byte: contextual property flags.
constexpr std::array<std::uint16_t, 3635U> kRangeValues{{
#include "unicode_line_break_values.inc"
}};

static_assert(kRangeEnds.size() == kRangeValues.size());
static_assert(kRangeEnds.back() == 0x10FFFFU);

} // namespace

LineBreakProperties line_break_properties(std::uint32_t code_point) noexcept {
    if (code_point > 0x10FFFFU) {
        return {};
    }
    std::size_t first = 0U;
    std::size_t limit = kRangeEnds.size();
    while (first < limit) {
        const std::size_t middle = first + (limit - first) / 2U;
        if (code_point <= kRangeEnds[middle]) {
            limit = middle;
        } else {
            first = middle + 1U;
        }
    }
    const std::uint16_t packed = kRangeValues[first];
    return {
        static_cast<LineBreakClass>(static_cast<std::uint8_t>(packed)),
        static_cast<std::uint8_t>(packed >> 8U)};
}

const char* line_break_class_name(LineBreakClass value) noexcept {
    switch (value) {
        case LineBreakClass::AL:
            return "AL";
        case LineBreakClass::BK:
            return "BK";
        case LineBreakClass::CR:
            return "CR";
        case LineBreakClass::LF:
            return "LF";
        case LineBreakClass::NL:
            return "NL";
        case LineBreakClass::SP:
            return "SP";
        case LineBreakClass::ZW:
            return "ZW";
        case LineBreakClass::ZWJ:
            return "ZWJ";
        case LineBreakClass::CM:
            return "CM";
        case LineBreakClass::WJ:
            return "WJ";
        case LineBreakClass::GL:
            return "GL";
        case LineBreakClass::B2:
            return "B2";
        case LineBreakClass::BA:
            return "BA";
        case LineBreakClass::BB:
            return "BB";
        case LineBreakClass::HH:
            return "HH";
        case LineBreakClass::HY:
            return "HY";
        case LineBreakClass::CB:
            return "CB";
        case LineBreakClass::CL:
            return "CL";
        case LineBreakClass::CP:
            return "CP";
        case LineBreakClass::EX:
            return "EX";
        case LineBreakClass::IN:
            return "IN";
        case LineBreakClass::NS:
            return "NS";
        case LineBreakClass::OP:
            return "OP";
        case LineBreakClass::QU:
            return "QU";
        case LineBreakClass::IS:
            return "IS";
        case LineBreakClass::NU:
            return "NU";
        case LineBreakClass::PO:
            return "PO";
        case LineBreakClass::PR:
            return "PR";
        case LineBreakClass::SY:
            return "SY";
        case LineBreakClass::H2:
            return "H2";
        case LineBreakClass::H3:
            return "H3";
        case LineBreakClass::HL:
            return "HL";
        case LineBreakClass::ID:
            return "ID";
        case LineBreakClass::JL:
            return "JL";
        case LineBreakClass::JV:
            return "JV";
        case LineBreakClass::JT:
            return "JT";
        case LineBreakClass::RI:
            return "RI";
        case LineBreakClass::EB:
            return "EB";
        case LineBreakClass::EM:
            return "EM";
        case LineBreakClass::AK:
            return "AK";
        case LineBreakClass::AP:
            return "AP";
        case LineBreakClass::AS:
            return "AS";
        case LineBreakClass::VF:
            return "VF";
        case LineBreakClass::VI:
            return "VI";
    }
    return "invalid";
}

} // namespace zevryon::text
