#include "html_tokenizer_data_stream_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using zevryon::massivedoc::HtmlTokenizerDataStreamV1Config;
using zevryon::massivedoc::HtmlTokenizerDataStreamV1Stats;
using zevryon::massivedoc::HtmlTokenizerV1ParseError;
using zevryon::massivedoc::HtmlTokenizerV1Sink;
using zevryon::massivedoc::HtmlTokenizerV1Token;
using zevryon::massivedoc::HtmlTokenizerV1TokenKind;
using zevryon::massivedoc::tokenize_html_data_stream_v1;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: HTML Data stream v1: " << message << '\n';
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

bool token_is_character(const HtmlTokenizerV1Token& token, std::string_view data) {
    return token.kind == HtmlTokenizerV1TokenKind::Character && token.data == data;
}

bool token_is_tag(
    const HtmlTokenizerV1Token& token,
    HtmlTokenizerV1TokenKind kind,
    std::string_view name) {
    return token.kind == kind && token.name == name;
}

bool test_ordered_composition() {
    const std::string input =
        "a<!--c--><H x=y>z</h><!DOCTYPE html>q";
    CollectingSink sink;
    HtmlTokenizerDataStreamV1Stats stats;
    std::string error;
    if (!require(
            tokenize_html_data_stream_v1(input, {}, &sink, &stats, &error),
            std::string("ordered composition: ") + error) ||
        !require(sink.tokens.size() == 7U, "ordered token count") ||
        !require(sink.errors.empty(), "ordered composition errors")) {
        return false;
    }

    return require(token_is_character(sink.tokens[0], "a"), "leading character") &&
        require(sink.tokens[1].kind == HtmlTokenizerV1TokenKind::Comment &&
                    sink.tokens[1].data == "c",
                "comment order") &&
        require(token_is_tag(sink.tokens[2], HtmlTokenizerV1TokenKind::StartTag, "h") &&
                    sink.tokens[2].attributes.size() == 1U &&
                    sink.tokens[2].attributes[0].name == "x" &&
                    sink.tokens[2].attributes[0].value == "y",
                "start tag order") &&
        require(token_is_character(sink.tokens[3], "z"), "middle character") &&
        require(token_is_tag(sink.tokens[4], HtmlTokenizerV1TokenKind::EndTag, "h"),
                "end tag order") &&
        require(sink.tokens[5].kind == HtmlTokenizerV1TokenKind::Doctype &&
                    sink.tokens[5].name == "html" && !sink.tokens[5].force_quirks,
                "doctype order") &&
        require(token_is_character(sink.tokens[6], "q"), "trailing character") &&
        require(stats.tokens_emitted == 7U, "token stats") &&
        require(stats.character_tokens_emitted == 3U &&
                    stats.character_bytes_emitted == 3U,
                "character stats") &&
        require(stats.start_tags_emitted == 1U && stats.end_tags_emitted == 1U,
                "tag stats") &&
        require(stats.comment_tokens_emitted == 1U &&
                    stats.doctype_tokens_emitted == 1U,
                "markup stats") &&
        require(stats.attributes_emitted == 1U, "attribute stats") &&
        require(stats.data_segments_consumed == 3U &&
                    stats.markup_declarations_consumed == 2U,
                "composition segment stats");
}

bool test_quoted_attribute_does_not_split_markup() {
    CollectingSink sink;
    HtmlTokenizerDataStreamV1Stats stats;
    std::string error;
    if (!require(
            tokenize_html_data_stream_v1(
                "<x a='<!DOC>'><!--real-->",
                {},
                &sink,
                &stats,
                &error),
            std::string("quoted markup-looking attribute: ") + error) ||
        !require(sink.tokens.size() == 2U, "quoted markup token count")) {
        return false;
    }
    return require(
               sink.tokens[0].kind == HtmlTokenizerV1TokenKind::StartTag &&
                   sink.tokens[0].attributes.size() == 1U &&
                   sink.tokens[0].attributes[0].value == "<!DOC>",
               "quoted declaration-looking bytes remain attribute data") &&
        require(sink.tokens[1].kind == HtmlTokenizerV1TokenKind::Comment &&
                    sink.tokens[1].data == "real",
                "real declaration still routed") &&
        require(sink.errors.empty(), "quoted markup-looking attribute has no errors") &&
        require(stats.data_segments_consumed == 1U &&
                    stats.markup_declarations_consumed == 1U,
                "quoted markup routing stats");
}

bool test_delegated_data_error_location_is_global() {
    const std::string input = "<!--c--><h a='b'c='d'>";
    CollectingSink sink;
    HtmlTokenizerDataStreamV1Stats stats;
    std::string error;
    if (!require(
            tokenize_html_data_stream_v1(input, {}, &sink, &stats, &error),
            std::string("global Data error location: ") + error) ||
        !require(sink.errors.size() == 1U, "global Data error count")) {
        return false;
    }
    return require(
               sink.errors[0].code == "missing-whitespace-between-attributes" &&
                   sink.errors[0].line == 1U && sink.errors[0].column == 17U,
               "delegated Data error translated to global column") &&
        require(stats.parse_errors_emitted == 1U, "global Data error stats");
}

bool test_markup_error_location_uses_original_input() {
    const std::string input = "<p>x</p>\n<!DOC>";
    CollectingSink sink;
    HtmlTokenizerDataStreamV1Stats stats;
    std::string error;
    if (!require(
            tokenize_html_data_stream_v1(input, {}, &sink, &stats, &error),
            std::string("global markup error location: ") + error) ||
        !require(sink.errors.size() == 1U, "global markup error count")) {
        return false;
    }
    return require(
               sink.errors[0].code == "incorrectly-opened-comment" &&
                   sink.errors[0].line == 2U && sink.errors[0].column == 3U,
               "markup helper retains global coordinates") &&
        require(sink.tokens.back().kind == HtmlTokenizerV1TokenKind::Comment &&
                    sink.tokens.back().data == "DOC",
                "bogus comment emitted in composed stream");
}

bool test_multiple_adjacent_declarations() {
    CollectingSink sink;
    HtmlTokenizerDataStreamV1Stats stats;
    std::string error;
    if (!require(
            tokenize_html_data_stream_v1(
                "<!--a--><!--b--><!DOCTYPE html>",
                {},
                &sink,
                &stats,
                &error),
            std::string("adjacent declarations: ") + error)) {
        return false;
    }
    return require(sink.tokens.size() == 3U, "adjacent declaration token count") &&
        require(sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Comment &&
                    sink.tokens[0].data == "a",
                "first adjacent comment") &&
        require(sink.tokens[1].kind == HtmlTokenizerV1TokenKind::Comment &&
                    sink.tokens[1].data == "b",
                "second adjacent comment") &&
        require(sink.tokens[2].kind == HtmlTokenizerV1TokenKind::Doctype &&
                    sink.tokens[2].name == "html",
                "adjacent doctype") &&
        require(stats.data_segments_consumed == 0U &&
                    stats.markup_declarations_consumed == 3U,
                "adjacent declaration accounting");
}

bool test_many_declarations_preserve_global_positions() {
    constexpr std::size_t kDeclarationCount = 2048U;
    std::string input;
    input.reserve(kDeclarationCount * 9U + 16U);
    for (std::size_t index = 0U; index < kDeclarationCount; ++index) {
        input += "<!--x-->\n";
    }
    input += "<h a='b'c='d'>";

    CollectingSink sink;
    HtmlTokenizerDataStreamV1Stats stats;
    std::string error;
    if (!require(
            tokenize_html_data_stream_v1(input, {}, &sink, &stats, &error),
            std::string("declaration-heavy global positions: ") + error) ||
        !require(sink.errors.size() == 1U, "declaration-heavy error count")) {
        return false;
    }

    return require(
               sink.errors[0].code == "missing-whitespace-between-attributes" &&
                   sink.errors[0].line == kDeclarationCount + 1U &&
                   sink.errors[0].column == 9U,
               "declaration-heavy error retains global coordinates") &&
        require(stats.markup_declarations_consumed == kDeclarationCount,
                "declaration-heavy markup accounting") &&
        require(stats.comment_tokens_emitted == kDeclarationCount,
                "declaration-heavy comment accounting") &&
        require(stats.data_segments_consumed == kDeclarationCount,
                "declaration-heavy Data segment accounting") &&
        require(stats.start_tags_emitted == 1U,
                "declaration-heavy final start-tag accounting");
}

bool test_admitted_references_fail_closed_surfaces_and_bounds() {
    {
        CollectingSink sink;
        HtmlTokenizerDataStreamV1Stats stats;
        std::string error;
        if (!require(
                tokenize_html_data_stream_v1("a&amp;b", {}, &sink, &stats, &error),
                std::string("named character reference is admitted: ") + error) ||
            !require(sink.tokens.size() == 1U, "named reference composed token count") ||
            !require(token_is_character(sink.tokens[0], "a&b"),
                     "named reference decodes through composed Data stream") ||
            !require(sink.errors.empty(), "named reference composed stream has no errors") ||
            !require(stats.data_segments_consumed == 1U,
                     "successful named-reference Data segment is counted") ||
            !require(stats.tokens_emitted == 1U &&
                         stats.character_tokens_emitted == 1U &&
                         stats.character_bytes_emitted == 3U,
                     "composed named-reference output stats use decoded bytes")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        HtmlTokenizerDataStreamV1Stats stats;
        std::string error;
        if (!require(
                tokenize_html_data_stream_v1(
                    "<!DOCTYPE html PUBLIC 'x'>",
                    {},
                    &sink,
                    &stats,
                    &error),
                std::string("PUBLIC doctype is admitted: ") + error) ||
            !require(sink.tokens.size() == 1U, "PUBLIC doctype token count") ||
            !require(sink.errors.empty(), "PUBLIC doctype has no parse errors") ||
            !require(
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Doctype &&
                    sink.tokens[0].name == "html" &&
                    sink.tokens[0].has_public_identifier &&
                    sink.tokens[0].public_identifier == "x" &&
                    !sink.tokens[0].has_system_identifier &&
                    sink.tokens[0].system_identifier.empty() &&
                    !sink.tokens[0].force_quirks,
                "PUBLIC doctype token payload") ||
            !require(stats.tokens_emitted == 1U &&
                         stats.doctype_tokens_emitted == 1U &&
                         stats.parse_errors_emitted == 0U,
                     "PUBLIC doctype token accounting") ||
            !require(stats.data_segments_consumed == 0U &&
                         stats.markup_declarations_consumed == 1U,
                     "PUBLIC doctype declaration accounting")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        HtmlTokenizerDataStreamV1Config config;
        config.maximum_token_bytes = 3U;
        HtmlTokenizerDataStreamV1Stats stats;
        std::string error;
        if (!require(
                !tokenize_html_data_stream_v1("<!--abcd-->", config, &sink, &stats, &error),
                "composed markup hard cap rejects oversized token") ||
            !require(error.find("comment token exceeds bounded byte limit") != std::string::npos,
                     "composed markup hard-cap error explicit") ||
            !require(sink.tokens.empty(), "composed markup hard cap publishes no token")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    if (!test_ordered_composition() ||
        !test_quoted_attribute_does_not_split_markup() ||
        !test_delegated_data_error_location_is_global() ||
        !test_markup_error_location_uses_original_input() ||
        !test_multiple_adjacent_declarations() ||
        !test_many_declarations_preserve_global_positions() ||
        !test_admitted_references_fail_closed_surfaces_and_bounds()) {
        return 1;
    }
    return 0;
}
