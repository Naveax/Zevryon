#include "unicode_bidi.hpp"

#include <array>
#include <cstddef>

namespace zevryon::text {
namespace {

template <typename Range, std::size_t Size>
const Range* find_range(
    const std::array<Range, Size>& ranges,
    std::uint32_t codepoint) noexcept {
    std::size_t first = 0U;
    std::size_t count = ranges.size();
    while (count != 0U) {
        const std::size_t step = count / 2U;
        const std::size_t middle = first + step;
        if (ranges[middle].end < codepoint) {
            first = middle + 1U;
            count -= step + 1U;
        } else {
            count = step;
        }
    }
    if (first < ranges.size() &&
        ranges[first].start <= codepoint &&
        codepoint <= ranges[first].end) {
        return &ranges[first];
    }
    return nullptr;
}

bool valid_class(BidiClass value) noexcept {
    return static_cast<std::size_t>(value) <
           static_cast<std::size_t>(BidiClass::Count);
}

} // namespace

BidiClass bidi_class_of(std::uint32_t codepoint) noexcept {
    if (codepoint > 0x10ffffU) {
        return BidiClass::L;
    }
    const BidiClassRange* range = find_range(kBidiClassRanges, codepoint);
    return range == nullptr ? BidiClass::L : range->value;
}

std::string_view bidi_class_short_name(BidiClass value) noexcept {
    const std::size_t index = static_cast<std::size_t>(value);
    return valid_class(value) ? kBidiClassShortNames[index] : std::string_view{};
}

std::string_view bidi_class_long_name(BidiClass value) noexcept {
    const std::size_t index = static_cast<std::size_t>(value);
    return valid_class(value) ? kBidiClassLongNames[index] : std::string_view{};
}

bool bidi_class_from_name(std::string_view name, BidiClass* value) noexcept {
    if (value == nullptr) {
        return false;
    }
    for (std::size_t index = 0U; index < kBidiClassShortNames.size(); ++index) {
        if (kBidiClassShortNames[index] == name ||
            kBidiClassLongNames[index] == name) {
            *value = static_cast<BidiClass>(index);
            return true;
        }
    }
    return false;
}

bool is_bidi_strong_left(BidiClass value) noexcept {
    return value == BidiClass::L;
}

bool is_bidi_strong_right(BidiClass value) noexcept {
    return value == BidiClass::R || value == BidiClass::AL;
}

bool is_bidi_isolate_initiator(BidiClass value) noexcept {
    return value == BidiClass::LRI ||
           value == BidiClass::RLI ||
           value == BidiClass::FSI;
}

bool is_bidi_explicit_formatting(BidiClass value) noexcept {
    return value == BidiClass::LRE ||
           value == BidiClass::LRO ||
           value == BidiClass::RLE ||
           value == BidiClass::RLO ||
           value == BidiClass::PDF ||
           is_bidi_isolate_initiator(value) ||
           value == BidiClass::PDI ||
           value == BidiClass::BN;
}

} // namespace zevryon::text
