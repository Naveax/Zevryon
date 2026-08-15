#include "massivedoc_unicode_search_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace {
using namespace zevryon::massivedoc::detail;

[[noreturn]] void die(std::string_view message) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
void require(bool value, std::string_view message) { if(!value) die(message); }

std::span<const std::byte> bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

void test_casefold_and_source_offset() {
    UnicodeSearchRuntime runtime({});
    std::string error;
    require(runtime.build_pattern("STRASSE", &error), "pattern STRASSE");
    runtime.reset_record();
    const std::string source = "xxStra\xC3\x9F" "e yy";
    require(runtime.feed_record(bytes(std::string_view(source).substr(0, 7)), 0U, &error), "first chunk");
    require(runtime.feed_record(bytes(std::string_view(source).substr(7)), 7U, &error), "second chunk");
    require(runtime.finish_record(&error), "finish source");
    require(runtime.found(), "casefold match");
    require(runtime.match_source_start() == 2U, "casefold source start");
    require(runtime.match_source_end() == 9U, "casefold source end");
}

void test_canonical_equivalence_split_utf8() {
    UnicodeSearchRuntime runtime({});
    std::string error;
    require(runtime.build_pattern("\xC3\x85LAND", &error), "pattern A-ring");
    runtime.reset_record();
    const std::string source = "A\xCC\x8Aland";
    require(runtime.feed_record(bytes(std::string_view(source).substr(0, 2)), 0U, &error), "split 1");
    require(runtime.feed_record(bytes(std::string_view(source).substr(2, 1)), 2U, &error), "split 2");
    require(runtime.feed_record(bytes(std::string_view(source).substr(3)), 3U, &error), "split 3");
    require(runtime.finish_record(&error), "finish canonical");
    require(runtime.found(), "canonical match");
    require(runtime.match_source_start() == 0U, "canonical source start");
    require(runtime.match_source_end() == source.size(), "canonical source end");
}

void test_invalid_utf8_query_and_record() {
    UnicodeSearchRuntime runtime({});
    std::string error;
    const std::string bad_query("\xC3", 1);
    require(!runtime.build_pattern(bad_query, &error), "truncated query should fail");
    require(error.find("invalid UTF-8") != std::string::npos, "query UTF8 error text");

    require(runtime.build_pattern("abc", &error), "valid query after reset");
    runtime.reset_record();
    const std::array<std::byte, 1> bad_record{std::byte{0xff}};
    require(!runtime.feed_record(bad_record, 0U, &error), "invalid record should fail");
    require(error.find("invalid UTF-8") != std::string::npos, "record UTF8 error text");
}

void test_query_limits() {
    UnicodeSearchRuntime runtime({4U, 4U, 8U});
    std::string error;
    require(!runtime.build_pattern("abcde", &error), "query byte bound");
    require(error.find("byte bound") != std::string::npos, "query byte bound text");

    UnicodeSearchRuntime fold_runtime({64U, 1U, 8U});
    require(!fold_runtime.build_pattern("\xC3\x9F", &error), "fold expansion cap");
    require(error.find("codepoint bound") != std::string::npos, "fold cap text");
}

void test_pending_limit() {
    UnicodeSearchRuntime runtime({64U, 32U, 2U});
    std::string error;
    require(runtime.build_pattern("x", &error), "pending pattern");
    runtime.reset_record();
    const std::string source = "a\xCC\x8A\xCC\x95";
    require(!runtime.feed_record(bytes(source), 0U, &error), "pending bound must fail");
    require(error.find("pending sequence") != std::string::npos, "pending bound text");
}

} // namespace

int main() {
    test_casefold_and_source_offset();
    test_canonical_equivalence_split_utf8();
    test_invalid_utf8_query_and_record();
    test_query_limits();
    test_pending_limit();
    std::cout << "Zevryon MassiveDoc Unicode search runtime tests passed\n";
    return 0;
}
