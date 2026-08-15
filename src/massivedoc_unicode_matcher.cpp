#include "massivedoc_unicode_matcher.hpp"

#include <algorithm>
#include <limits>

namespace zevryon::massivedoc {

bool build_unicode_search_pattern(
    std::span<const text::NormalizedSearchCodePoint> normalized_query,
    UnicodeSearchPatternConfig config,
    UnicodeSearchPattern* pattern,
    UnicodeSearchPatternError* error) {
    if (pattern == nullptr) {
        return false;
    }
    pattern->codepoints.clear();
    pattern->prefix.clear();
    if (error != nullptr) {
        *error = {};
    }
    if (normalized_query.empty()) {
        if (error != nullptr) {
            error->kind = UnicodeSearchPatternErrorKind::EmptyPattern;
            error->message = "normalized search pattern is empty";
        }
        return false;
    }
    if (config.max_codepoints == 0U || normalized_query.size() > config.max_codepoints) {
        if (error != nullptr) {
            error->kind = UnicodeSearchPatternErrorKind::PatternLimitExceeded;
            error->message = "normalized search pattern exceeds configured codepoint bound";
        }
        return false;
    }

    pattern->codepoints.reserve(normalized_query.size());
    for (const auto& value : normalized_query) {
        pattern->codepoints.push_back(value.value);
    }
    pattern->prefix.assign(pattern->codepoints.size(), 0U);
    for (std::size_t index = 1U; index < pattern->codepoints.size(); ++index) {
        std::size_t candidate = pattern->prefix[index - 1U];
        while (candidate != 0U &&
               pattern->codepoints[index] != pattern->codepoints[candidate]) {
            candidate = pattern->prefix[candidate - 1U];
        }
        if (pattern->codepoints[index] == pattern->codepoints[candidate]) {
            ++candidate;
        }
        pattern->prefix[index] = candidate;
    }
    return true;
}

UnicodeSearchMatcher::UnicodeSearchMatcher(const UnicodeSearchPattern& pattern)
    : pattern_(pattern), ring_(pattern.codepoints.size()) {}

void UnicodeSearchMatcher::reset() noexcept {
    matched_ = 0U;
    seen_ = 0U;
    match_ = {};
}

bool UnicodeSearchMatcher::feed(
    std::span<const text::NormalizedSearchCodePoint> values) {
    if (match_.found || pattern_.codepoints.empty()) {
        return match_.found;
    }
    for (const auto& value : values) {
        const std::size_t ring_index = static_cast<std::size_t>(
            seen_ % static_cast<std::uint64_t>(ring_.size()));
        ring_[ring_index] = SourceSpan{value.source_start, value.source_end};
        ++seen_;

        while (matched_ != 0U && value.value != pattern_.codepoints[matched_]) {
            matched_ = pattern_.prefix[matched_ - 1U];
        }
        if (value.value == pattern_.codepoints[matched_]) {
            ++matched_;
        }
        if (matched_ == pattern_.codepoints.size()) {
            record_match();
            return true;
        }
    }
    return false;
}

void UnicodeSearchMatcher::record_match() {
    const std::uint64_t width = static_cast<std::uint64_t>(ring_.size());
    const std::uint64_t first_sequence_index = seen_ - width;
    std::uint64_t minimum_start = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t maximum_end = 0U;
    for (std::uint64_t offset = 0U; offset < width; ++offset) {
        const std::uint64_t sequence_index = first_sequence_index + offset;
        const std::size_t ring_index = static_cast<std::size_t>(sequence_index % width);
        minimum_start = std::min(minimum_start, ring_[ring_index].start);
        maximum_end = std::max(maximum_end, ring_[ring_index].end);
    }
    match_.found = true;
    match_.source_start = minimum_start;
    match_.source_end = maximum_end;
}

const UnicodeSearchMatch& UnicodeSearchMatcher::match() const noexcept {
    return match_;
}

std::uint64_t UnicodeSearchMatcher::normalized_codepoints_seen() const noexcept {
    return seen_;
}

} // namespace zevryon::massivedoc
