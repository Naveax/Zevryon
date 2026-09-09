#include "html_tokenizer_markup_declarations_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using zevryon::massivedoc::HtmlTokenizerMarkupDeclarationsV1Config;
using zevryon::massivedoc::HtmlTokenizerMarkupDeclarationsV1Stats;
using zevryon::massivedoc::HtmlTokenizerV1ParseError;
using zevryon::massivedoc::HtmlTokenizerV1Sink;
using zevryon::massivedoc::HtmlTokenizerV1Token;
using zevryon::massivedoc::HtmlTokenizerV1TokenKind;
using zevryon::massivedoc::consume_html_markup_declaration_v1;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: HTML markup declarations v1: " << message << '\n';
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

struct ExpectedError {
    std::string code;
    std::uint64_t line{1U};
    std::uint64_t column{1U};
};

bool check_errors(
    const CollectingSink& sink,
    const std::vector<ExpectedError>& expected,
    std::string_view label) {
    if (!require(
            sink.errors.size() == expected.size(),
            std::string(label) + " parse-error count")) {
        return false;
    }
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        if (!require(
                sink.errors[index].code == expected[index].code &&
                    sink.errors[index].line == expected[index].line &&
                    sink.errors[index].column == expected[index].column,
                std::string(label) + " parse-error at index " +
                    std::to_string(index))) {
            return false;
        }
    }
    return true;
}

bool run_doctype_case(
    std::string_view label,
    std::string_view input,
    std::string_view expected_name,
    bool expected_force_quirks,
    const std::vector<ExpectedError>& expected_errors = {}) {
    CollectingSink sink;
    HtmlTokenizerMarkupDeclarationsV1Stats stats;
    std::size_t next_offset = 0U;
    std::string error;
    if (!require(
            consume_html_markup_declaration_v1(
                input,
                0U,
                {},
                &sink,
                &stats,
                &next_offset,
                &error),
            std::string(label) + ": " + error) ||
        !require(next_offset == input.size(), std::string(label) + " consumes input") ||
        !require(sink.tokens.size() == 1U, std::string(label) + " token count") ||
        !check_errors(sink, expected_errors, label)) {
        return false;
    }
    const HtmlTokenizerV1Token& token = sink.tokens[0];
    return require(token.kind == HtmlTokenizerV1TokenKind::Doctype, std::string(label) + " kind") &&
        require(token.name == expected_name, std::string(label) + " normalized name") &&
        require(!token.has_public_identifier && !token.has_system_identifier,
                std::string(label) + " null identifiers") &&
        require(token.public_identifier.empty() && token.system_identifier.empty(),
                std::string(label) + " identifier payloads empty") &&
        require(token.force_quirks == expected_force_quirks,
                std::string(label) + " force-quirks") &&
        require(stats.tokens_emitted == 1U && stats.doctype_tokens_emitted == 1U &&
                    stats.comment_tokens_emitted == 0U &&
                    stats.parse_errors_emitted == expected_errors.size(),
                std::string(label) + " stats");
}

bool run_comment_case(
    std::string_view label,
    std::string_view input,
    std::string_view expected_data,
    const std::vector<ExpectedError>& expected_errors = {}) {
    CollectingSink sink;
    HtmlTokenizerMarkupDeclarationsV1Stats stats;
    std::size_t next_offset = 0U;
    std::string error;
    if (!require(
            consume_html_markup_declaration_v1(
                input,
                0U,
                {},
                &sink,
                &stats,
                &next_offset,
                &error),
            std::string(label) + ": " + error) ||
        !require(next_offset == input.size(), std::string(label) + " consumes input") ||
        !require(sink.tokens.size() == 1U, std::string(label) + " token count") ||
        !check_errors(sink, expected_errors, label)) {
        return false;
    }
    const HtmlTokenizerV1Token& token = sink.tokens[0];
    return require(token.kind == HtmlTokenizerV1TokenKind::Comment,
                   std::string(label) + " kind") &&
        require(token.data == expected_data, std::string(label) + " data") &&
        require(stats.tokens_emitted == 1U && stats.comment_tokens_emitted == 1U &&
                    stats.doctype_tokens_emitted == 0U &&
                    stats.parse_errors_emitted == expected_errors.size(),
                std::string(label) + " stats");
}

bool test_pinned_test1_doctypes() {
    return run_doctype_case("Correct Doctype lowercase", "<!DOCTYPE html>", "html", false) &&
        run_doctype_case("Correct Doctype uppercase", "<!DOCTYPE HTML>", "html", false) &&
        run_doctype_case("Correct Doctype mixed case", "<!DOCTYPE HtMl>", "html", false) &&
        run_doctype_case(
            "Correct Doctype case with EOF",
            "<!DOCTYPE HtMl",
            "html",
            true,
            {ExpectedError{"eof-in-doctype", 1U, 15U}}) &&
        run_doctype_case("Doctype in error", "<!DOCTYPE foo>", "foo", false);
}

bool test_pinned_test1_comments() {
    return run_comment_case(
               "Truncated doctype start",
               "<!DOC>",
               "DOC",
               {ExpectedError{"incorrectly-opened-comment", 1U, 3U}}) &&
        run_comment_case("Simple comment", "<!--comment-->", "comment") &&
        run_comment_case("Comment central dash", "<!----->", "-") &&
        run_comment_case("Comment two central dashes", "<!-- --comment -->", " --comment ") &&
        run_comment_case("Comment central less-than bang", "<!--<!-->", "<!") &&
        run_comment_case(
            "Unfinished comment",
            "<!--comment",
            "comment",
            {ExpectedError{"eof-in-comment", 1U, 12U}}) &&
        run_comment_case(
            "Unfinished nested-comment start",
            "<!-- <!--",
            " <!",
            {ExpectedError{"eof-in-comment", 1U, 10U}}) &&
        run_comment_case(
            "Start of a comment",
            "<!-",
            "-",
            {ExpectedError{"incorrectly-opened-comment", 1U, 3U}}) &&
        run_comment_case(
            "Short comment",
            "<!-->",
            "",
            {ExpectedError{"abrupt-closing-of-empty-comment", 1U, 5U}}) &&
        run_comment_case(
            "Short comment two",
            "<!--->",
            "",
            {ExpectedError{"abrupt-closing-of-empty-comment", 1U, 6U}}) &&
        run_comment_case("Short comment three", "<!---->", "") &&
        run_comment_case("Less-than in comment", "<!-- <test-->", " <test") &&
        run_comment_case("Double less-than in comment", "<!--<<-->", "<<") &&
        run_comment_case("Less-than bang in comment", "<!-- <!test-->", " <!test") &&
        run_comment_case("Less-than bang dash in comment", "<!-- <!-test-->", " <!-test") &&
        run_comment_case(
            "Nested comment",
            "<!-- <!--test-->",
            " <!--test",
            {ExpectedError{"nested-comment", 1U, 10U}}) &&
        run_comment_case(
            "Nested comment with extra less-than",
            "<!-- <<!--test-->",
            " <<!--test",
            {ExpectedError{"nested-comment", 1U, 11U}});
}

bool test_nonzero_offset_preserves_global_location() {
    const std::string input = "prefix\n<!DOC>tail";
    const std::size_t offset = input.find("<!");
    CollectingSink sink;
    HtmlTokenizerMarkupDeclarationsV1Stats stats;
    std::size_t next_offset = 0U;
    std::string error;
    if (!require(
            consume_html_markup_declaration_v1(
                input, offset, {}, &sink, &stats, &next_offset, &error),
            std::string("nonzero offset: ") + error)) {
        return false;
    }
    return require(sink.tokens.size() == 1U && sink.tokens[0].data == "DOC",
                   "nonzero offset bogus-comment data") &&
        require(sink.errors.size() == 1U, "nonzero offset error count") &&
        require(sink.errors[0].code == "incorrectly-opened-comment" &&
                    sink.errors[0].line == 2U && sink.errors[0].column == 3U,
                "nonzero offset global error location") &&
        require(next_offset == input.find("tail"), "nonzero offset next cursor");
}

bool test_fail_closed_boundaries() {
    {
        CollectingSink sink;
        HtmlTokenizerMarkupDeclarationsV1Config config;
        config.maximum_token_bytes = 3U;
        HtmlTokenizerMarkupDeclarationsV1Stats stats;
        std::size_t next_offset = 99U;
        std::string error;
        if (!require(
                !consume_html_markup_declaration_v1(
                    "<!--abcd-->", 0U, config, &sink, &stats, &next_offset, &error),
                "comment hard cap rejects oversized token") ||
            !require(error.find("comment token exceeds bounded byte limit") != std::string::npos,
                     "comment hard-cap error explicit") ||
            !require(sink.tokens.empty(), "comment hard cap publishes no token")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        HtmlTokenizerMarkupDeclarationsV1Stats stats;
        std::size_t next_offset = 0U;
        std::string error;
        if (!require(
                !consume_html_markup_declaration_v1(
                    "<!DOCTYPE html PUBLIC 'x'>",
                    0U,
                    {},
                    &sink,
                    &stats,
                    &next_offset,
                    &error),
                "PUBLIC identifier remains fail closed") ||
            !require(error.find("PUBLIC/SYSTEM") != std::string::npos,
                     "PUBLIC failure identifies open recovery surface") ||
            !require(sink.tokens.empty() && sink.errors.empty(),
                     "unsupported DOCTYPE publishes no events")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        const std::string input("<!--a\0b-->", 10U);
        HtmlTokenizerMarkupDeclarationsV1Stats stats;
        std::size_t next_offset = 0U;
        std::string error;
        if (!require(
                !consume_html_markup_declaration_v1(
                    input, 0U, {}, &sink, &stats, &next_offset, &error),
                "comment NUL remains fail closed") ||
            !require(error.find("preprocessing/NUL replacement") != std::string::npos,
                     "comment NUL failure explicit") ||
            !require(sink.tokens.empty(), "comment NUL publishes no token")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    if (!test_pinned_test1_doctypes() ||
        !test_pinned_test1_comments() ||
        !test_nonzero_offset_preserves_global_location() ||
        !test_fail_closed_boundaries()) {
        return 1;
    }
    return 0;
}
