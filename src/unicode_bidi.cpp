#include "unicode_bidi.hpp"

#include <array>
#include <cstddef>

namespace zevryon::text {
namespace {

template <typename Range, std::size_t Size>
const Range* find_interval(
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
    return first < ranges.size() &&
                   ranges[first].start <= codepoint &&
                   codepoint <= ranges[first].end
        ? &ranges[first]
        : nullptr;
}

template <typename Entry, std::size_t Size>
const Entry* find_codepoint(
    const std::array<Entry, Size>& entries,
    std::uint32_t codepoint) noexcept {
    std::size_t first = 0U;
    std::size_t count = entries.size();
    while (count != 0U) {
        const std::size_t step = count / 2U;
        const std::size_t middle = first + step;
        if (entries[middle].codepoint < codepoint) {
            first = middle + 1U;
            count -= step + 1U;
        } else {
            count = step;
        }
    }
    return first < entries.size() && entries[first].codepoint == codepoint
        ? &entries[first]
        : nullptr;
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
    const BidiClassRange* range = find_interval(kBidiClassRanges, codepoint);
    return range == nullptr ? BidiClass::L : range->value;
}

BidiBracketInfo bidi_bracket_of(std::uint32_t codepoint) noexcept {
    if (codepoint > 0x10ffffU) {
        return {};
    }
    const BidiBracketEntry* entry = find_codepoint(kBidiBracketEntries, codepoint);
    return entry == nullptr
        ? BidiBracketInfo{}
        : BidiBracketInfo{entry->paired, entry->type};
}

bool bidi_mirror_of(std::uint32_t codepoint, std::uint32_t* mirror) noexcept {
    if (mirror == nullptr || codepoint > 0x10ffffU) {
        return false;
    }
    const BidiMirrorEntry* entry = find_codepoint(kBidiMirrorEntries, codepoint);
    if (entry == nullptr) {
        *mirror = codepoint;
        return false;
    }
    *mirror = entry->mirror;
    return true;
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

} // namespace zevryon::text
