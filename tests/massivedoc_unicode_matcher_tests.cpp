#include "massivedoc_unicode_matcher.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>

namespace {
using namespace zevryon::massivedoc;
using zevryon::text::NormalizedSearchCodePoint;

[[noreturn]] void die(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}
void require(bool value, std::string_view message) {
    if (!value) die(message);
}

UnicodeSearchPattern pattern_from(std::span<const NormalizedSearchCodePoint> values) {
    UnicodeSearchPattern pattern;
    UnicodeSearchPatternError error;
    require(build_unicode_search_pattern(values, {}, &pattern, &error), "pattern build failed");
    return pattern;
}

void test_basic_match_and_offset() {
    const std::array<NormalizedSearchCodePoint, 3> query{{
        {0x61U, 0U, 1U}, {0x62U, 1U, 2U}, {0x63U, 2U, 3U}}};
    const auto pattern = pattern_from(query);
    UnicodeSearchMatcher matcher(pattern);
    const std::array<NormalizedSearchCodePoint, 5> source{{
        {0x78U, 0U, 1U}, {0x61U, 4U, 5U}, {0x62U, 5U, 6U},
        {0x63U, 6U, 7U}, {0x79U, 7U, 8U}}};
    require(matcher.feed(source), "basic match not found");
    require(matcher.match().source_start == 4U, "basic start offset");
    require(matcher.match().source_end == 7U, "basic end offset");
}

void test_fold_expansion_span() {
    const std::array<NormalizedSearchCodePoint, 2> query{{
        {0x73U, 0U, 1U}, {0x73U, 1U, 2U}}};
    const auto pattern = pattern_from(query);
    UnicodeSearchMatcher matcher(pattern);
    const std::array<NormalizedSearchCodePoint, 2> source{{
        {0x73U, 10U, 12U}, {0x73U, 10U, 12U}}};
    require(matcher.feed(source), "fold expansion match not found");
    require(matcher.match().source_start == 10U, "fold expansion start");
    require(matcher.match().source_end == 12U, "fold expansion end");
}

void test_reordered_source_spans_use_minimum_start() {
    const std::array<NormalizedSearchCodePoint, 2> query{{
        {0x300U, 0U, 1U}, {0x315U, 1U, 2U}}};
    const auto pattern = pattern_from(query);
    UnicodeSearchMatcher matcher(pattern);
    const std::array<NormalizedSearchCodePoint, 2> source{{
        {0x300U, 20U, 22U}, {0x315U, 18U, 20U}}};
    require(matcher.feed(source), "reordered match not found");
    require(matcher.match().source_start == 18U, "reordered minimum source start");
    require(matcher.match().source_end == 22U, "reordered maximum source end");
}

void test_kmp_fallback_across_feeds() {
    const std::array<NormalizedSearchCodePoint, 4> query{{
        {1U,0U,1U}, {2U,1U,2U}, {1U,2U,3U}, {3U,3U,4U}}};
    const auto pattern = pattern_from(query);
    UnicodeSearchMatcher matcher(pattern);
    const std::array<NormalizedSearchCodePoint, 3> first{{
        {1U,0U,1U}, {2U,1U,2U}, {1U,2U,3U}}};
    const std::array<NormalizedSearchCodePoint, 3> second{{
        {2U,3U,4U}, {1U,4U,5U}, {3U,5U,6U}}};
    require(!matcher.feed(first), "premature KMP match");
    require(matcher.feed(second), "KMP fallback match not found");
    require(matcher.match().source_start == 2U, "KMP match start");
    require(matcher.match().source_end == 6U, "KMP match end");
}

void test_pattern_cap_and_empty_fail_closed() {
    UnicodeSearchPattern pattern;
    UnicodeSearchPatternError error;
    const std::array<NormalizedSearchCodePoint, 2> query{{
        {1U,0U,1U}, {2U,1U,2U}}};
    require(!build_unicode_search_pattern(query, {1U}, &pattern, &error), "cap must reject");
    require(error.kind == UnicodeSearchPatternErrorKind::PatternLimitExceeded, "cap error kind");
    require(!build_unicode_search_pattern({}, {}, &pattern, &error), "empty must reject");
    require(error.kind == UnicodeSearchPatternErrorKind::EmptyPattern, "empty error kind");
}

} // namespace

int main() {
    test_basic_match_and_offset();
    test_fold_expansion_span();
    test_reordered_source_spans_use_minimum_start();
    test_kmp_fallback_across_feeds();
    test_pattern_cap_and_empty_fail_closed();
    std::cout << "Zevryon MassiveDoc Unicode matcher tests passed\n";
    return 0;
}
