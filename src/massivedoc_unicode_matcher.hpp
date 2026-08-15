#pragma once

#include "unicode_search_normalizer.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace zevryon::massivedoc {

struct UnicodeSearchPatternConfig {
    std::size_t max_codepoints{4096U};
};

enum class UnicodeSearchPatternErrorKind : std::uint8_t {
    None = 0,
    EmptyPattern,
    PatternLimitExceeded,
};

struct UnicodeSearchPatternError {
    UnicodeSearchPatternErrorKind kind{UnicodeSearchPatternErrorKind::None};
    std::string_view message;
};

struct UnicodeSearchPattern {
    std::vector<std::uint32_t> codepoints;
    std::vector<std::size_t> prefix;
};

bool build_unicode_search_pattern(
    std::span<const text::NormalizedSearchCodePoint> normalized_query,
    UnicodeSearchPatternConfig config,
    UnicodeSearchPattern* pattern,
    UnicodeSearchPatternError* error);

struct UnicodeSearchMatch {
    bool found{false};
    std::uint64_t source_start{0};
    std::uint64_t source_end{0};
};

class UnicodeSearchMatcher {
public:
    explicit UnicodeSearchMatcher(const UnicodeSearchPattern& pattern);

    bool feed(std::span<const text::NormalizedSearchCodePoint> values);
    void reset() noexcept;
    const UnicodeSearchMatch& match() const noexcept;
    std::uint64_t normalized_codepoints_seen() const noexcept;

private:
    struct SourceSpan {
        std::uint64_t start{0};
        std::uint64_t end{0};
    };

    void record_match();

    const UnicodeSearchPattern& pattern_;
    std::vector<SourceSpan> ring_;
    std::size_t matched_{0U};
    std::uint64_t seen_{0U};
    UnicodeSearchMatch match_{};
};

} // namespace zevryon::massivedoc
