#include "html_tokenizer_character_reference_v1.hpp"
#include "html_tokenizer_data_tags_v1.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using zevryon::massivedoc::HtmlTokenizerCharacterReferenceV1Context;
using zevryon::massivedoc::HtmlTokenizerCharacterReferenceV1Result;
using zevryon::massivedoc::HtmlTokenizerCharacterReferenceV1Stats;
using zevryon::massivedoc::HtmlTokenizerDataTagsV1Stats;
using zevryon::massivedoc::HtmlTokenizerV1Config;
using zevryon::massivedoc::HtmlTokenizerV1ParseError;
using zevryon::massivedoc::HtmlTokenizerV1Sink;
using zevryon::massivedoc::HtmlTokenizerV1Token;
using zevryon::massivedoc::HtmlTokenizerV1TokenKind;
using zevryon::massivedoc::consume_html_character_reference_v1;
using zevryon::massivedoc::tokenize_html_data_tags_v1;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: numeric character-reference v1: " << message << '\n';
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

bool run_reference(
    std::string_view input,
    HtmlTokenizerCharacterReferenceV1Context context,
    CollectingSink* sink,
    HtmlTokenizerCharacterReferenceV1Stats* stats,
    HtmlTokenizerCharacterReferenceV1Result* result,
    std::string* error) {
    return consume_html_character_reference_v1(
        input,
        0U,
        context,
        sink,
        stats,
        result,
        error);
}

bool test_literal_ampersand_fallback() {
    for (const std::string& input : {std::string("&"), std::string("&&"), std::string("& ")}) {
        CollectingSink sink;
        HtmlTokenizerCharacterReferenceV1Stats stats;
        HtmlTokenizerCharacterReferenceV1Result result;
        std::string error;
        if (!require(
                run_reference(
                    input,
                    HtmlTokenizerCharacterReferenceV1Context::Data,
                    &sink,
                    &stats,
                    &result,
                    &error),
                std::string("literal fallback: ") + error) ||
            !require(result.replacement_utf8 == "&", "literal replacement") ||
            !require(result.next_offset == 1U, "literal fallback reconsume offset") ||
            !require(sink.errors.empty(), "literal fallback parse errors") ||
            !require(stats.parse_errors_emitted == 0U, "literal fallback stats")) {
            return false;
        }
    }
    return true;
}

bool test_absence_of_digits_recovery() {
    struct Case {
        std::string input;
        std::string replacement;
        std::size_t next_offset;
        std::uint64_t column;
    };
    const Case cases[] = {
        {"&#", "&#", 2U, 3U},
        {"&#x", "&#x", 3U, 4U},
        {"&#X;", "&#X", 3U, 4U},
    };

    for (const Case& item : cases) {
        CollectingSink sink;
        HtmlTokenizerCharacterReferenceV1Stats stats;
        HtmlTokenizerCharacterReferenceV1Result result;
        std::string error;
        if (!require(
                run_reference(
                    item.input,
                    HtmlTokenizerCharacterReferenceV1Context::Data,
                    &sink,
                    &stats,
                    &result,
                    &error),
                std::string("absence of digits: ") + error) ||
            !require(result.replacement_utf8 == item.replacement, "absence replacement") ||
            !require(result.next_offset == item.next_offset, "absence reconsume offset") ||
            !require(sink.errors.size() == 1U, "absence error count") ||
            !require(
                sink.errors[0].code == "absence-of-digits-in-numeric-character-reference" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == item.column,
                "absence exact parse-error") ||
            !require(stats.parse_errors_emitted == 1U, "absence stats")) {
            return false;
        }
    }
    return true;
}

bool test_decimal_hex_and_missing_semicolon() {
    {
        CollectingSink sink;
        HtmlTokenizerCharacterReferenceV1Stats stats;
        HtmlTokenizerCharacterReferenceV1Result result;
        std::string error;
        if (!require(
                run_reference(
                    "&#0036;",
                    HtmlTokenizerCharacterReferenceV1Context::Data,
                    &sink,
                    &stats,
                    &result,
                    &error),
                std::string("decimal: ") + error) ||
            !require(result.replacement_utf8 == "$" && result.next_offset == 7U,
                     "decimal result") ||
            !require(sink.errors.empty(), "decimal errors")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        HtmlTokenizerCharacterReferenceV1Stats stats;
        HtmlTokenizerCharacterReferenceV1Result result;
        std::string error;
        if (!require(
                run_reference(
                    "&#x3f;",
                    HtmlTokenizerCharacterReferenceV1Context::Attribute,
                    &sink,
                    &stats,
                    &result,
                    &error),
                std::string("hex: ") + error) ||
            !require(result.replacement_utf8 == "?" && result.next_offset == 6U,
                     "hex result") ||
            !require(sink.errors.empty(), "hex errors")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        HtmlTokenizerCharacterReferenceV1Stats stats;
        HtmlTokenizerCharacterReferenceV1Result result;
        std::string error;
        if (!require(
                run_reference(
                    "&#65x",
                    HtmlTokenizerCharacterReferenceV1Context::Data,
                    &sink,
                    &stats,
                    &result,
                    &error),
                std::string("missing semicolon: ") + error) ||
            !require(result.replacement_utf8 == "A" && result.next_offset == 4U,
                     "missing-semicolon result/reconsume") ||
            !require(sink.errors.size() == 1U, "missing-semicolon error count") ||
            !require(
                sink.errors[0].code == "missing-semicolon-after-character-reference" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == 5U,
                "missing-semicolon exact error")) {
            return false;
        }
    }
    return true;
}

bool test_numeric_end_state_rules() {
    struct Case {
        std::string input;
        std::string expected;
        std::string error_code;
    };
    const std::string replacement("\xEF\xBF\xBD", 3U);
    const std::string euro("\xE2\x82\xAC", 3U);
    const std::string noncharacter("\xEF\xB7\x90", 3U); // U+FDD0
    const Case cases[] = {
        {"&#0;", replacement, "null-character-reference"},
        {"&#xD800;", replacement, "surrogate-character-reference"},
        {"&#1114112;", replacement, "character-reference-outside-unicode-range"},
        {"&#xFDD0;", noncharacter, "noncharacter-character-reference"},
        {"&#x80;", euro, "control-character-reference"},
    };

    for (const Case& item : cases) {
        CollectingSink sink;
        HtmlTokenizerCharacterReferenceV1Stats stats;
        HtmlTokenizerCharacterReferenceV1Result result;
        std::string error;
        if (!require(
                run_reference(
                    item.input,
                    HtmlTokenizerCharacterReferenceV1Context::Data,
                    &sink,
                    &stats,
                    &result,
                    &error),
                std::string("numeric end state: ") + error) ||
            !require(result.replacement_utf8 == item.expected, "numeric end replacement") ||
            !require(result.next_offset == item.input.size(), "numeric end offset") ||
            !require(sink.errors.size() == 1U, "numeric end error count") ||
            !require(sink.errors[0].code == item.error_code, "numeric end error code") ||
            !require(stats.parse_errors_emitted == 1U, "numeric end stats")) {
            return false;
        }
    }
    return true;
}

bool test_named_candidate_uses_shared_table() {
    CollectingSink sink;
    HtmlTokenizerCharacterReferenceV1Stats stats;
    HtmlTokenizerCharacterReferenceV1Result result;
    std::string error;
    const std::string not_sign("\xC2\xAC", 2U);
    return require(
               run_reference(
                   "&not;",
                   HtmlTokenizerCharacterReferenceV1Context::Data,
                   &sink,
                   &stats,
                   &result,
                   &error),
               std::string("named reference dispatch: ") + error) &&
        require(result.replacement_utf8 == not_sign && result.next_offset == 5U,
                "named reference replacement") &&
        require(sink.errors.empty() && sink.tokens.empty(), "named reference diagnostics") &&
        require(stats.parse_errors_emitted == 0U, "named reference stats");
}

bool test_data_and_attribute_integration() {
    {
        const std::string input = "A&&&#0036;&#x3f;Z";
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Stats stats;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error),
                std::string("Data integration: ") + error) ||
            !require(sink.tokens.size() == 1U, "Data integration token count") ||
            !require(sink.errors.empty(), "Data integration errors") ||
            !require(
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                    sink.tokens[0].data == "A&&$?Z",
                "Data integration replacement/coalescing")) {
            return false;
        }
    }
    {
        const std::string euro("\xE2\x82\xAC", 3U);
        const std::string input = "<h a='x&#x80;y' b=&#63;></h>";
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Stats stats;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error),
                std::string("attribute integration: ") + error) ||
            !require(sink.tokens.size() == 2U, "attribute integration token count") ||
            !require(sink.errors.size() == 1U, "attribute integration error count") ||
            !require(
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::StartTag &&
                    sink.tokens[0].attributes.size() == 2U &&
                    sink.tokens[0].attributes[0].name == "a" &&
                    sink.tokens[0].attributes[0].value == std::string("x") + euro + "y" &&
                    sink.tokens[0].attributes[1].name == "b" &&
                    sink.tokens[0].attributes[1].value == "?",
                "attribute integration replacements") ||
            !require(
                sink.errors[0].code == "control-character-reference",
                "attribute integration C1 error") ||
            !require(stats.parse_errors_emitted == 1U, "attribute integration stats")) {
            return false;
        }
    }
    {
        const std::string input = "<s o=& t><a a=a&>foo";
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Stats stats;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error),
                std::string("attribute ampersand fallback: ") + error) ||
            !require(sink.errors.empty(), "attribute ampersand errors") ||
            !require(sink.tokens.size() == 3U, "attribute ampersand token count") ||
            !require(
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::StartTag &&
                    sink.tokens[0].attributes.size() == 2U &&
                    sink.tokens[0].attributes[0].name == "o" &&
                    sink.tokens[0].attributes[0].value == "&" &&
                    sink.tokens[0].attributes[1].name == "t" &&
                    sink.tokens[0].attributes[1].value.empty(),
                "unquoted ampersand fallback") ||
            !require(
                sink.tokens[1].kind == HtmlTokenizerV1TokenKind::StartTag &&
                    sink.tokens[1].attributes.size() == 1U &&
                    sink.tokens[1].attributes[0].value == "a&",
                "ampersand before tag close") ||
            !require(
                sink.tokens[2].kind == HtmlTokenizerV1TokenKind::Character &&
                    sink.tokens[2].data == "foo",
                "post-tag character data")) {
            return false;
        }
    }
    return true;
}

bool test_replacement_respects_token_byte_cap() {
    CollectingSink sink;
    HtmlTokenizerDataTagsV1Stats stats;
    zevryon::massivedoc::HtmlTokenizerDataTagsV1Config config;
    config.maximum_token_bytes = 2U;
    std::string error;
    return require(
               !tokenize_html_data_tags_v1("&#x20AC;", config, &sink, &stats, &error),
               "three-byte replacement must exceed two-byte cap") &&
        require(error.find("coalesced character token exceeds bounded byte limit") !=
                    std::string::npos,
                "replacement hard-cap failure explicit") &&
        require(sink.tokens.empty(), "replacement hard cap publishes no token");
}

} // namespace

int main() {
    return test_literal_ampersand_fallback() &&
            test_absence_of_digits_recovery() &&
            test_decimal_hex_and_missing_semicolon() &&
            test_numeric_end_state_rules() &&
            test_named_candidate_uses_shared_table() &&
            test_data_and_attribute_integration() &&
            test_replacement_respects_token_byte_cap()
        ? 0
        : 1;
}
