#include "html_tokenizer_data_tags_v1.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using zevryon::massivedoc::HtmlTokenizerDataTagsV1Stats;
using zevryon::massivedoc::HtmlTokenizerV1ParseError;
using zevryon::massivedoc::HtmlTokenizerV1Sink;
using zevryon::massivedoc::HtmlTokenizerV1Token;
using zevryon::massivedoc::HtmlTokenizerV1TokenKind;
using zevryon::massivedoc::tokenize_html_data_tags_v1;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: HTML Data tag recovery v1: " << message << '\n';
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

    bool on_parse_error(
        const HtmlTokenizerV1ParseError& parse_error,
        std::string*) override {
        errors.push_back(parse_error);
        return true;
    }

    std::vector<HtmlTokenizerV1Token> tokens;
    std::vector<HtmlTokenizerV1ParseError> errors;
};

bool test_empty_start_tag_reconsumes_in_data() {
    CollectingSink sink;
    HtmlTokenizerDataTagsV1Stats stats;
    std::string error;
    if (!require(
            tokenize_html_data_tags_v1("<>", {}, &sink, &stats, &error),
            std::string("empty start tag recovery: ") + error) ||
        !require(sink.tokens.size() == 1U, "empty start tag token count") ||
        !require(sink.errors.size() == 1U, "empty start tag error count")) {
        return false;
    }

    return require(
               sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                   sink.tokens[0].data == "<>",
               "empty start tag becomes Data character bytes") &&
        require(
            sink.errors[0].code == "invalid-first-character-of-tag-name" &&
                sink.errors[0].line == 1U && sink.errors[0].column == 2U,
            "empty start tag exact parse-error location") &&
        require(stats.tokens_emitted == 1U, "empty start tag token stats") &&
        require(stats.character_tokens_emitted == 1U &&
                    stats.character_bytes_emitted == 2U,
                "empty start tag character stats") &&
        require(stats.parse_errors_emitted == 1U, "empty start tag error stats");
}

bool test_invalid_ascii_tag_open_reconsumes_generally() {
    CollectingSink sink;
    HtmlTokenizerDataTagsV1Stats stats;
    std::string error;
    if (!require(
            tokenize_html_data_tags_v1("<1x", {}, &sink, &stats, &error),
            std::string("invalid ASCII tag-open recovery: ") + error) ||
        !require(sink.tokens.size() == 1U, "invalid ASCII tag-open token count") ||
        !require(sink.errors.size() == 1U, "invalid ASCII tag-open error count")) {
        return false;
    }

    return require(
               sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                   sink.tokens[0].data == "<1x",
               "invalid ASCII tag-open bytes are reconsumed in Data") &&
        require(
            sink.errors[0].code == "invalid-first-character-of-tag-name" &&
                sink.errors[0].line == 1U && sink.errors[0].column == 2U,
            "invalid ASCII tag-open exact parse-error location") &&
        require(stats.parse_errors_emitted == 1U, "invalid ASCII tag-open error stats");
}

bool test_empty_end_tag_reports_and_discards() {
    CollectingSink sink;
    HtmlTokenizerDataTagsV1Stats stats;
    std::string error;
    if (!require(
            tokenize_html_data_tags_v1("</>", {}, &sink, &stats, &error),
            std::string("empty end tag recovery: ") + error) ||
        !require(sink.tokens.empty(), "empty end tag emits no token") ||
        !require(sink.errors.size() == 1U, "empty end tag error count")) {
        return false;
    }

    return require(
               sink.errors[0].code == "missing-end-tag-name" &&
                   sink.errors[0].line == 1U && sink.errors[0].column == 3U,
               "empty end tag exact parse-error location") &&
        require(stats.tokens_emitted == 0U, "empty end tag token stats") &&
        require(stats.parse_errors_emitted == 1U, "empty end tag error stats");
}

bool test_unquoted_special_bytes_are_diagnosed_and_preserved() {
    constexpr std::array<char, 5U> specials = {'"', '\'', '<', '=', '`'};
    for (char special : specials) {
        std::string input = "<a a=f";
        input.push_back(special);
        input.push_back('>');

        CollectingSink sink;
        HtmlTokenizerDataTagsV1Stats stats;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error),
                std::string("unquoted special recovery: ") + error) ||
            !require(sink.tokens.size() == 1U, "unquoted special token count") ||
            !require(sink.errors.size() == 1U, "unquoted special error count")) {
            return false;
        }

        std::string expected_value = "f";
        expected_value.push_back(special);
        const HtmlTokenizerV1Token& token = sink.tokens[0];
        if (!require(
                token.kind == HtmlTokenizerV1TokenKind::StartTag &&
                    token.name == "a" && token.attributes.size() == 1U,
                "unquoted special start-tag shape") ||
            !require(
                token.attributes[0].name == "a" &&
                    token.attributes[0].value == expected_value,
                "unquoted special byte remains in attribute value") ||
            !require(
                sink.errors[0].code ==
                        "unexpected-character-in-unquoted-attribute-value" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == 7U,
                "unquoted special exact parse-error location") ||
            !require(stats.tokens_emitted == 1U && stats.start_tags_emitted == 1U,
                     "unquoted special token stats") ||
            !require(stats.attributes_emitted == 1U,
                     "unquoted special attribute stats") ||
            !require(stats.parse_errors_emitted == 1U,
                     "unquoted special parse-error stats")) {
            return false;
        }
    }
    return true;
}

bool test_new_recovery_does_not_admit_preprocessing_debt() {
    {
        CollectingSink sink;
        const std::string input("<\0>", 3U);
        std::string error;
        if (!require(
                !tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                "tag-open NUL remains fail closed") ||
            !require(
                error.find("preprocessing/NUL replacement") != std::string::npos,
                "tag-open NUL failure remains explicit") ||
            !require(sink.tokens.empty() && sink.errors.empty(),
                     "tag-open NUL publishes no recovery events")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        const std::string input("<\xC3\xA9>", 4U);
        std::string error;
        if (!require(
                !tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                "tag-open non-ASCII remains fail closed") ||
            !require(
                error.find("non-ASCII preprocessing/location authority") !=
                    std::string::npos,
                "tag-open non-ASCII failure remains explicit") ||
            !require(sink.tokens.empty() && sink.errors.empty(),
                     "tag-open non-ASCII publishes no recovery events")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    if (!test_empty_start_tag_reconsumes_in_data() ||
        !test_invalid_ascii_tag_open_reconsumes_generally() ||
        !test_empty_end_tag_reports_and_discards() ||
        !test_unquoted_special_bytes_are_diagnosed_and_preserved() ||
        !test_new_recovery_does_not_admit_preprocessing_debt()) {
        return 1;
    }
    return 0;
}
