#include "unicode_bidi_brackets.hpp"

#include <algorithm>

namespace zevryon::text {

BidiBracketInfo bidi_bracket_info(std::uint32_t codepoint) noexcept {
    const auto iterator = std::lower_bound(
        kUnicodeBidiBracketRecords.begin(),
        kUnicodeBidiBracketRecords.end(),
        codepoint,
        [](const BidiBracketRecord& record, std::uint32_t value) {
            return record.codepoint < value;
        });
    if (iterator == kUnicodeBidiBracketRecords.end() ||
        iterator->codepoint != codepoint) {
        return {};
    }
    return BidiBracketInfo{iterator->paired_codepoint, iterator->type};
}

bool bidi_brackets_match(
    std::uint32_t expected_closing,
    std::uint32_t actual_closing) noexcept {
    if (expected_closing == actual_closing) {
        return true;
    }
    constexpr std::uint32_t kRightAngleBracket = 0x3009U;
    constexpr std::uint32_t kRightPointingAngleBracket = 0x232AU;
    return (expected_closing == kRightAngleBracket ||
            expected_closing == kRightPointingAngleBracket) &&
           (actual_closing == kRightAngleBracket ||
            actual_closing == kRightPointingAngleBracket);
}

} // namespace zevryon::text
