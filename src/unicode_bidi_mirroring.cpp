#include "unicode_bidi_mirroring.hpp"

#include <algorithm>

namespace zevryon::text {

BidiMirroringInfo bidi_mirroring_info(std::uint32_t codepoint) noexcept {
    const auto range = std::lower_bound(
        kUnicodeBidiMirroredRanges.begin(),
        kUnicodeBidiMirroredRanges.end(),
        codepoint,
        [](const BidiMirroredRange& candidate, std::uint32_t value) {
            return candidate.last < value;
        });
    if (range == kUnicodeBidiMirroredRanges.end() ||
        codepoint < range->first || codepoint > range->last) {
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
