#include "html_tokenizer_data_tags_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using zevryon::massivedoc::HtmlTokenizerDataTagsV1Config;
using zevryon::massivedoc::HtmlTokenizerDataTagsV1Stats;
using zevryon::massivedoc::HtmlTokenizerV1Attribute;
using zevryon::massivedoc::HtmlTokenizerV1ParseError;
using zevryon::massivedoc::HtmlTokenizerV1Sink;
using zevryon::massivedoc::HtmlTokenizerV1Token;
using zevryon::massivedoc::HtmlTokenizerV1TokenKind;
using zevryon::massivedoc::tokenize_html_data_tags_v1;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: HTML Data-tag tokenizer v1: " << message << '\n';
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

bool attribute_is(
    const HtmlTokenizerV1Attribute& attribute,
    std::string_view name,
    std::string_view value) {
    return attribute.name == name && attribute.value == value;
}

bool test_start_end_and_character_tokens() {
    CollectingSink sink;
    HtmlTokenizerDataTagsV1Stats stats;
    std::string error;
    if (!require(
            tokenize_html_data_tags_v1(
                "<H>one</h>",
                {},
                &sink,
                &stats,
                &error),
            std::string("basic tokenization: ") + error)) {
        return false;
    }
    return require(sink.tokens.size() == 3U, "basic token count") &&
        require(
            sink.tokens[0].kind == HtmlTokenizerV1TokenKind::StartTag &&
                sink.tokens[0].name == "h" &&
                sink.tokens[0].attributes.empty() &&
                !sink.tokens[0].self_closing,
            "start tag is normalized") &&
        require(
            sink.tokens[1].kind == HtmlTokenizerV1TokenKind::Character &&
                sink.tokens[1].data == "one",
            "character payload preserved") &&
        require(
            sink.tokens[2].kind == HtmlTokenizerV1TokenKind::EndTag &&
                sink.tokens[2].name == "h",
            "end tag is normalized") &&
        require(sink.errors.empty(), "basic tokenization has no parse errors") &&
        require(stats.tokens_emitted == 3U, "basic token stats") &&
        require(stats.start_tags_emitted == 1U, "basic start-tag stats") &&
        require(stats.end_tags_emitted == 1U, "basic end-tag stats") &&
        require(stats.character_tokens_emitted == 1U, "basic character-token stats") &&
        require(stats.character_bytes_emitted == 3U, "basic character-byte stats");
}

bool test_attributes_and_self_closing() {
    CollectingSink sink;
    HtmlTokenizerDataTagsV1Stats stats;
    std::string error;
    if (!require(
            tokenize_html_data_tags_v1(
                "<X A='b' c=DeF disabled checked/>",
                {},
                &sink,
                &stats,
                &error),
            std::string("attribute tokenization: ") + error)) {
        return false;
    }
    if (!require(sink.tokens.size() == 1U, "attribute token count")) {
        return false;
    }
    const HtmlTokenizerV1Token& token = sink.tokens[0];
    return require(
               token.kind == HtmlTokenizerV1TokenKind::StartTag &&
                   token.name == "x" && token.self_closing,
               "self-closing start tag") &&
        require(token.attributes.size() == 4U, "four attributes emitted") &&
        require(attribute_is(token.attributes[0], "a", "b"), "single-quoted attribute") &&
        require(attribute_is(token.attributes[1], "c", "DeF"), "unquoted attribute") &&
        require(attribute_is(token.attributes[2], "disabled", ""), "first empty attribute") &&
        require(attribute_is(token.attributes[3], "checked", ""), "second empty attribute") &&
        require(sink.errors.empty(), "attribute case has no parse errors") &&
        require(stats.attributes_emitted == 4U, "attribute stats") &&
        require(stats.start_tags_emitted == 1U, "attribute start-tag stats");
}

bool test_missing_whitespace_error_position() {
    CollectingSink sink;
    std::string error;
    if (!require(
            tokenize_html_data_tags_v1(
                "<h a='b'c='d'>",
                {},
                &sink,
                nullptr,
                &error),
            std::string("missing whitespace case: ") + error)) {
        return false;
    }
    return require(sink.tokens.size() == 1U, "missing whitespace token count") &&
        require(sink.tokens[0].attributes.size() == 2U, "missing whitespace attributes survive") &&
        require(sink.errors.size() == 1U, "missing whitespace emits one parse error") &&
        require(
            sink.errors[0].code == "missing-whitespace-between-attributes" &&
                sink.errors[0].line == 1U && sink.errors[0].column == 9U,
            "missing whitespace error position matches html5lib authority");
}

bool test_duplicate_attribute_first_wins() {
    CollectingSink sink;
    std::string error;
    if (!require(
            tokenize_html_data_tags_v1(
                "<h a='b' a='d'>",
                {},
                &sink,
                nullptr,
                &error),
            std::string("duplicate attribute case: ") + error)) {
        return false;
    }
    return require(sink.tokens.size() == 1U, "duplicate token count") &&
        require(sink.tokens[0].attributes.size() == 1U, "duplicate attribute dropped") &&
        require(attribute_is(sink.tokens[0].attributes[0], "a", "b"), "first duplicate value wins") &&
        require(sink.errors.size() == 1U, "duplicate emits one parse error") &&
        require(
            sink.errors[0].code == "duplicate-attribute" &&
                sink.errors[0].line == 1U && sink.errors[0].column == 11U,
            "duplicate error position matches html5lib authority");
}

bool test_end_tag_attributes_are_diagnosed_and_dropped() {
    CollectingSink sink;
    std::string error;
    if (!require(
            tokenize_html_data_tags_v1(
                "<h></h a='b'>",
                {},
                &sink,
                nullptr,
                &error),
            std::string("end-tag attribute case: ") + error)) {
        return false;
    }
    return require(sink.tokens.size() == 2U, "end-tag attribute token count") &&
        require(
            sink.tokens[1].kind == HtmlTokenizerV1TokenKind::EndTag &&
                sink.tokens[1].name == "h" &&
                sink.tokens[1].attributes.empty() &&
                !sink.tokens[1].self_closing,
            "end-tag attributes are not published") &&
        require(sink.errors.size() == 1U, "end-tag attributes emit one parse error") &&
        require(
            sink.errors[0].code == "end-tag-with-attributes" &&
                sink.errors[0].line == 1U && sink.errors[0].column == 13U,
            "end-tag attribute error position matches html5lib authority");
}

bool test_plaintext_start_tag_does_not_fake_tree_builder_feedback() {
    CollectingSink sink;
    std::string error;
    if (!require(
            tokenize_html_data_tags_v1(
                "<plaintext>foobar",
                {},
                &sink,
                nullptr,
                &error),
            std::string("plaintext start tag case: ") + error)) {
        return false;
    }
    return require(sink.tokens.size() == 2U, "plaintext Data token count") &&
        require(
            sink.tokens[0].kind == HtmlTokenizerV1TokenKind::StartTag &&
                sink.tokens[0].name == "plaintext",
            "plaintext start tag emitted") &&
        require(
            sink.tokens[1].kind == HtmlTokenizerV1TokenKind::Character &&
                sink.tokens[1].data == "foobar",
            "Data tokenizer does not self-switch to PLAINTEXT") &&
        require(sink.errors.empty(), "plaintext token case has no errors");
}

bool test_attribute_name_state_recovery() {
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("<z =>", {}, &sink, nullptr, &error),
                std::string("equals-name recovery: ") + error)) {
            return false;
        }
        if (!require(sink.tokens.size() == 1U, "equals-name token count") ||
            !require(sink.tokens[0].attributes.size() == 1U, "equals-name attribute count") ||
            !require(attribute_is(sink.tokens[0].attributes[0], "=", ""), "equals-name payload") ||
            !require(sink.errors.size() == 1U, "equals-name error count") ||
            !require(
                sink.errors[0].code == "unexpected-equals-sign-before-attribute-name" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == 4U,
                "equals-name error position")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("<z ==>", {}, &sink, nullptr, &error),
                std::string("missing attribute value recovery: ") + error)) {
            return false;
        }
        if (!require(sink.tokens.size() == 1U, "missing-value token count") ||
            !require(sink.tokens[0].attributes.size() == 1U, "missing-value attribute count") ||
            !require(attribute_is(sink.tokens[0].attributes[0], "=", ""), "missing-value payload") ||
            !require(sink.errors.size() == 2U, "missing-value error count") ||
            !require(
                sink.errors[0].code == "unexpected-equals-sign-before-attribute-name" &&
                    sink.errors[0].column == 4U,
                "missing-value first error") ||
            !require(
                sink.errors[1].code == "missing-attribute-value" &&
                    sink.errors[1].column == 6U,
                "missing-value second error")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("<foo \"='bar'>", {}, &sink, nullptr, &error),
                std::string("quoted-byte attribute name: ") + error)) {
            return false;
        }
        if (!require(sink.tokens.size() == 1U, "quoted-name token count") ||
            !require(sink.tokens[0].attributes.size() == 1U, "quoted-name attribute count") ||
            !require(attribute_is(sink.tokens[0].attributes[0], "\"", "bar"), "quoted-name payload") ||
            !require(sink.errors.size() == 1U, "quoted-name error count") ||
            !require(
                sink.errors[0].code == "unexpected-character-in-attribute-name" &&
                    sink.errors[0].column == 6U,
                "quoted-name error position")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string input = "<a a=''";
        input.push_back('\x0B');
        input.push_back('>');
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                std::string("control attribute name: ") + error)) {
            return false;
        }
        const std::string control_name(1U, '\x0B');
        if (!require(sink.tokens.size() == 1U, "control-name token count") ||
            !require(sink.tokens[0].attributes.size() == 2U, "control-name attribute count") ||
            !require(attribute_is(sink.tokens[0].attributes[0], "a", ""), "control-name first attribute") ||
            !require(attribute_is(sink.tokens[0].attributes[1], control_name, ""), "control-name payload") ||
            !require(sink.errors.size() == 2U, "control-name error count") ||
            !require(
                sink.errors[0].code == "control-character-in-input-stream" &&
                    sink.errors[0].column == 8U,
                "control-name input error order") ||
            !require(
                sink.errors[1].code == "missing-whitespace-between-attributes" &&
                    sink.errors[1].column == 8U,
                "control-name whitespace error order")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("<a a<>", {}, &sink, nullptr, &error),
                std::string("less-than attribute name: ") + error)) {
            return false;
        }
        return require(sink.tokens.size() == 1U, "less-than token count") &&
            require(sink.tokens[0].attributes.size() == 1U, "less-than attribute count") &&
            require(attribute_is(sink.tokens[0].attributes[0], "a<", ""), "less-than name retained") &&
            require(sink.errors.size() == 1U, "less-than error count") &&
            require(
                sink.errors[0].code == "unexpected-character-in-attribute-name" &&
                    sink.errors[0].column == 5U,
                "less-than error position");
    }
}

bool test_tag_name_and_self_closing_state_recovery() {
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("<a<b>", {}, &sink, nullptr, &error),
                std::string("less-than tag-name state: ") + error)) {
            return false;
        }
        if (!require(sink.tokens.size() == 1U, "less-than tag-name token count") ||
            !require(
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::StartTag &&
                    sink.tokens[0].name == "a<b" &&
                    sink.tokens[0].attributes.empty(),
                "less-than byte remains in tag name") ||
            !require(sink.errors.empty(), "less-than tag-name emits no parse error")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("<h/a='b'>", {}, &sink, nullptr, &error),
                std::string("solidus reconsume: ") + error)) {
            return false;
        }
        if (!require(sink.tokens.size() == 1U, "solidus reconsume token count") ||
            !require(sink.tokens[0].name == "h", "solidus reconsume tag name") ||
            !require(!sink.tokens[0].self_closing, "solidus reconsume is not self-closing") ||
            !require(sink.tokens[0].attributes.size() == 1U, "solidus reconsume attribute count") ||
            !require(attribute_is(sink.tokens[0].attributes[0], "a", "b"), "solidus reconsume attribute") ||
            !require(sink.errors.size() == 1U, "solidus reconsume error count") ||
            !require(
                sink.errors[0].code == "unexpected-solidus-in-tag" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == 4U,
                "solidus reconsume error position")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string input = "<a/";
        input.push_back('\x0B');
        input.push_back('>');
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                std::string("solidus control reconsume: ") + error)) {
            return false;
        }
        const std::string control_name(1U, '\x0B');
        if (!require(sink.tokens.size() == 1U, "solidus control token count") ||
            !require(sink.tokens[0].attributes.size() == 1U, "solidus control attribute count") ||
            !require(attribute_is(sink.tokens[0].attributes[0], control_name, ""), "solidus control attribute") ||
            !require(sink.errors.size() == 2U, "solidus control error count") ||
            !require(
                sink.errors[0].code == "control-character-in-input-stream" &&
                    sink.errors[0].column == 4U,
                "solidus control input error is first") ||
            !require(
                sink.errors[1].code == "unexpected-solidus-in-tag" &&
                    sink.errors[1].column == 4U,
                "solidus tokenizer error follows input error")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string input = "<a";
        input.push_back('\x0B');
        input.push_back('>');
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                std::string("tag-name control byte: ") + error)) {
            return false;
        }
        std::string expected_name = "a";
        expected_name.push_back('\x0B');
        if (!require(sink.tokens.size() == 1U, "tag-name control token count") ||
            !require(sink.tokens[0].name == expected_name, "tag-name control byte retained") ||
            !require(sink.errors.size() == 1U, "tag-name control error count") ||
            !require(
                sink.errors[0].code == "control-character-in-input-stream" &&
                    sink.errors[0].column == 3U,
                "tag-name control error position")) {
            return false;
        }
    }
    return true;
}

bool test_admitted_references_and_fail_closed_boundaries() {
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                !tokenize_html_data_tags_v1(
                    "<!DOCTYPE html>", {}, &sink, nullptr, &error),
                "DOCTYPE remains fail-closed") ||
            !require(
                error.find("comments/DOCTYPE/markup declarations") != std::string::npos,
                "DOCTYPE failure is explicit")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Stats stats;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1(
                    "a&amp;b", {}, &sink, &stats, &error),
                std::string("named character reference is admitted: ") + error) ||
            !require(sink.tokens.size() == 1U, "named reference token count") ||
            !require(
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                    sink.tokens[0].data == "a&b",
                "named reference decodes inside coalesced Data character token") ||
            !require(sink.errors.empty(), "named reference emits no parse errors") ||
            !require(stats.tokens_emitted == 1U &&
                         stats.character_tokens_emitted == 1U &&
                         stats.character_bytes_emitted == 3U,
                     "named reference output stats use decoded bytes")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        const std::string input("a\0b", 3U);
        std::string error;
        if (!require(
                !tokenize_html_data_tags_v1(
                    input, {}, &sink, nullptr, &error),
                "NUL preprocessing remains fail-closed") ||
            !require(
                error.find("preprocessing/NUL replacement") != std::string::npos,
                "NUL failure is explicit")) {
            return false;
        }
    }
    return true;
}

bool test_token_and_attribute_bounds() {
    {
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Config config;
        config.maximum_token_bytes = 3U;
        std::string error;
        if (!require(
                !tokenize_html_data_tags_v1(
                    "abcd", config, &sink, nullptr, &error),
                "character hard cap rejects oversized coalesced token") ||
            !require(
                error.find("coalesced character token exceeds bounded byte limit") !=
                    std::string::npos,
                "character hard-cap error is explicit") ||
            !require(sink.tokens.empty(), "character hard cap emits no partial token")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Config config;
        config.maximum_attributes = 1U;
        std::string error;
        if (!require(
                !tokenize_html_data_tags_v1(
                    "<h a=b c=d>", config, &sink, nullptr, &error),
                "attribute hard cap rejects second attribute") ||
            !require(
                error.find("attribute count exceeds bounded limit") != std::string::npos,
                "attribute hard-cap error is explicit") ||
            !require(sink.tokens.empty(), "attribute hard cap publishes no tag token")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    if (!test_start_end_and_character_tokens() ||
        !test_attributes_and_self_closing() ||
        !test_missing_whitespace_error_position() ||
        !test_duplicate_attribute_first_wins() ||
        !test_attribute_name_state_recovery() ||
        !test_tag_name_and_self_closing_state_recovery() ||
        !test_end_tag_attributes_are_diagnosed_and_dropped() ||
        !test_plaintext_start_tag_does_not_fake_tree_builder_feedback() ||
        !test_admitted_references_and_fail_closed_boundaries() ||
        !test_token_and_attribute_bounds()) {
        return 1;
    }
    return 0;
}
