#include "html_tokenizer_data_tags_v1.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using zevryon::massivedoc::HtmlTokenizerDataTagsV1Config;
using zevryon::massivedoc::HtmlTokenizerDataTagsV1Stats;
using zevryon::massivedoc::HtmlTokenizerV1ParseError;
using zevryon::massivedoc::HtmlTokenizerV1Sink;
using zevryon::massivedoc::HtmlTokenizerV1Token;
using zevryon::massivedoc::HtmlTokenizerV1TokenKind;
using zevryon::massivedoc::tokenize_html_data_tags_v1;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: Data UTF-8 text v1: " << message << '\n';
        return false;
    }
    return true;
}

class CollectingSink final : public HtmlTokenizerV1Sink {
public:
    bool on_token(const HtmlTokenizerV1Token& token, std::string*) override {
        tokens.push_back(token);
        return true;
    }
    bool on_parse_error(const HtmlTokenizerV1ParseError& value, std::string*) override {
        errors.push_back(value);
        return true;
    }
    std::vector<HtmlTokenizerV1Token> tokens;
    std::vector<HtmlTokenizerV1ParseError> errors;
};

bool test_valid_scalar_passthrough() {
    const std::string input = std::string("A") +
        std::string("\xC2\xAC", 2U) +
        std::string("\xE2\x82\xAC", 3U) +
        std::string("\xF0\x9F\x98\x80", 4U) + "Z";
    CollectingSink sink;
    HtmlTokenizerDataTagsV1Stats stats;
    std::string error;
    return require(tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error), error) &&
        require(sink.tokens.size() == 1U, "valid UTF-8 emits one coalesced token") &&
        require(sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character,
                "valid UTF-8 emits Character token") &&
        require(sink.tokens[0].data == input, "valid UTF-8 bytes preserved exactly") &&
        require(sink.errors.empty(), "valid UTF-8 emits no diagnostics") &&
        require(stats.input_bytes == input.size(), "input byte accounting") &&
        require(stats.character_bytes_emitted == input.size(), "output byte accounting");
}

bool test_non_ascii_after_ampersand_is_literal_data() {
    const std::string input = std::string("&") + std::string("\xC2\xAC", 2U) + ";";
    CollectingSink sink;
    HtmlTokenizerDataTagsV1Stats stats;
    std::string error;
    return require(tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error), error) &&
        require(sink.tokens.size() == 1U &&
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                sink.tokens[0].data == input,
                "non-name Unicode scalar after ampersand remains literal") &&
        require(sink.errors.empty() && stats.parse_errors_emitted == 0U,
                "literal Unicode ampersand fallback diagnostics");
}

bool test_scalar_aware_error_columns() {
    const std::string input = std::string("\xC2\xAC", 2U) + "<q a=`>";
    CollectingSink sink;
    HtmlTokenizerDataTagsV1Stats stats;
    std::string error;
    return require(tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error), error) &&
        require(sink.errors.size() == 1U, "Unicode-prefix parse-error count") &&
        require(sink.errors[0].code == "unexpected-character-in-unquoted-attribute-value",
                "Unicode-prefix parse-error code") &&
        require(sink.errors[0].line == 1U && sink.errors[0].column == 7U,
                "Unicode-prefix column counts scalar, not UTF-8 bytes");
}

bool test_invalid_utf8_fails_closed() {
    const std::vector<std::string> invalid = {
        std::string("\x80", 1U),
        std::string("\xC0\x80", 2U),
        std::string("\xC2", 1U),
        std::string("\xC2\x41", 2U),
        std::string("\xE0\x80\x80", 3U),
        std::string("\xED\xA0\x80", 3U),
        std::string("\xF0\x80\x80\x80", 4U),
        std::string("\xF4\x90\x80\x80", 4U),
        std::string("\xF5\x80\x80\x80", 4U),
    };
    for (const std::string& input : invalid) {
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Stats stats;
        std::string error;
        if (!require(!tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error),
                     "malformed UTF-8 must fail") ||
            !require(error.find("invalid UTF-8 scalar encoding") != std::string::npos,
                     "malformed UTF-8 failure is explicit") ||
            !require(sink.tokens.empty() && sink.errors.empty(),
                     "malformed UTF-8 publishes no partial events")) {
            return false;
        }
    }
    return true;
}

bool test_utf8_respects_token_byte_cap() {
    CollectingSink sink;
    HtmlTokenizerDataTagsV1Stats stats;
    HtmlTokenizerDataTagsV1Config config;
    config.maximum_token_bytes = 1U;
    std::string error;
    const std::string input("\xC2\xAC", 2U);
    return require(!tokenize_html_data_tags_v1(input, config, &sink, &stats, &error),
                   "two-byte scalar exceeds one-byte token cap") &&
        require(error.find("coalesced character token exceeds bounded byte limit") != std::string::npos,
                "UTF-8 cap failure remains explicit") &&
        require(sink.tokens.empty(), "UTF-8 cap failure publishes no token");
}

bool test_scope_guards_remain_fail_closed() {
    const std::string scalar("\xC2\xAC", 2U);
    {
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Stats stats;
        std::string error;
        const std::string input = "<" + scalar + ">";
        if (!require(!tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error),
                     "non-ASCII tag name remains outside slice") ||
            !require(error.find("non-ASCII preprocessing/location authority") != std::string::npos,
                     "non-ASCII tag-name guard remains explicit")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Stats stats;
        std::string error;
        const std::string input = "<h a='" + scalar + "'>";
        if (!require(!tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error),
                     "non-ASCII attribute value remains outside slice") ||
            !require(error.find("non-ASCII attribute-value authority") != std::string::npos,
                     "non-ASCII attribute-value guard remains explicit")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Stats stats;
        std::string error;
        const std::string input("A\0B", 3U);
        if (!require(!tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error),
                     "NUL preprocessing remains outside slice") ||
            !require(error.find("NUL replacement is not implemented") != std::string::npos,
                     "NUL guard remains explicit")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    return test_valid_scalar_passthrough() &&
            test_non_ascii_after_ampersand_is_literal_data() &&
            test_scalar_aware_error_columns() &&
            test_invalid_utf8_fails_closed() &&
            test_utf8_respects_token_byte_cap() &&
            test_scope_guards_remain_fail_closed()
        ? 0
        : 1;
}
