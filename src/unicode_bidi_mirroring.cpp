#include "unicode_bidi_mirroring.hpp"

#include <algorithm>

namespace zevryon::text {
namespace {

const BidiMirroredRange* find_mirrored_range(std::uint32_t codepoint) noexcept {
    const auto range = std::lower_bound(
        kUnicodeBidiMirroredRanges.begin(),
        kUnicodeBidiMirroredRanges.end(),
        codepoint,
        [](const BidiMirroredRange& candidate, std::uint32_t value) {
            return candidate.last < value;
        });
    return range != kUnicodeBidiMirroredRanges.end() &&
                   codepoint >= range->first && codepoint <= range->last
        ? &*range
        : nullptr;
}

} // namespace

bool bidi_mirrored_property(std::uint32_t codepoint) noexcept {
    return find_mirrored_range(codepoint) != nullptr;
}

BidiMirroringInfo bidi_mirroring_info(std::uint32_t codepoint) noexcept {
    if (!bidi_mirrored_property(codepoint)) {
        return {};
    }

    BidiMirroringInfo info;
    info.mirrored = true;
    const auto mapping = std::lower_bound(
        kUnicodeBidiMirroringGlyphs.begin(),
        kUnicodeBidiMirroringGlyphs.end(),
        codepoint,
        [](const BidiMirroringGlyphRecord& candidate, std::uint32_t value) {
            return candidate.codepoint < value;
        });
    if (mapping != kUnicodeBidiMirroringGlyphs.end() &&
        mapping->codepoint == codepoint) {
        info.has_character_mapping = true;
        info.best_fit = mapping->best_fit;
        info.mirror_codepoint = mapping->mirror_codepoint;
    }
    return info;
}

} // namespace zevryon::text
