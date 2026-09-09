#include "html_tokenizer_token_stream_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using zevryon::massivedoc::HtmlTokenizerV1Config;
using zevryon::massivedoc::HtmlTokenizerV1InitialState;
using zevryon::massivedoc::HtmlTokenizerV1ParseError;
using zevryon::massivedoc::HtmlTokenizerV1Sink;
using zevryon::massivedoc::HtmlTokenizerV1Stats;
using zevryon::massivedoc::HtmlTokenizerV1Token;
using zevryon::massivedoc::HtmlTokenizerV1TokenKind;
using zevryon::massivedoc::tokenize_html_token_stream_v1;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: HTML tokenizer token stream v1: " << message << '\n';
        return false;
    }
    return true;
}

struct ExpectedToken {
    HtmlTokenizerV1TokenKind kind{HtmlTokenizerV1TokenKind::Character};
    std::string value;
};

struct ExpectedError {
    std::string code;
    std::uint64_t line{1U};
    std::uint64_t column{1U};
};

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

const char* state_name(HtmlTokenizerV1InitialState state) {
    switch (state) {
    case HtmlTokenizerV1InitialState::Plaintext:
        return "PLAINTEXT";
    case HtmlTokenizerV1InitialState::Rcdata:
        return "RCDATA";
    case HtmlTokenizerV1InitialState::Rawtext:
        return "RAWTEXT";
    }
    return "unknown";
}

bool run_case(
    std::string_view description,
    HtmlTokenizerV1InitialState state,
    std::string_view last_start_tag,
    std::string_view input,
    const std::vector<ExpectedToken>& expected_tokens,
    const std::vector<ExpectedError>& expected_errors) {
    CollectingSink sink;
    HtmlTokenizerV1Stats stats;
    std::string error;
    if (!require(
            tokenize_html_token_stream_v1(
                input,
                state,
                last_start_tag,
                {},
                &sink,
                &stats,
                &error),
            std::string(description) + " [" + state_name(state) + "]: " + error)) {
        return false;
    }
    if (!require(
            sink.tokens.size() == expected_tokens.size(),
            std::string(description) + " token count") ||
        !require(
            sink.errors.size() == expected_errors.size(),
            std::string(description) + " parse-error count") ||
        !require(
            stats.tokens_emitted == expected_tokens.size(),
            std::string(description) + " token stats") ||
        !require(
            stats.parse_errors_emitted == expected_errors.size(),
            std::string(description) + " parse-error stats") ||
        !require(
            stats.input_bytes == input.size(),
            std::string(description) + " input-byte stats")) {
        return false;
    }

    for (std::size_t index = 0U; index < expected_tokens.size(); ++index) {
        const HtmlTokenizerV1Token& actual = sink.tokens[index];
        const ExpectedToken& expected = expected_tokens[index];
        if (!require(
                actual.kind == expected.kind,
                std::string(description) + " token kind at index " +
                    std::to_string(index))) {
            return false;
        }
        if (expected.kind == HtmlTokenizerV1TokenKind::Character) {
            if (!require(
                    actual.data == expected.value && actual.name.empty(),
                    std::string(description) + " character token at index " +
                        std::to_string(index))) {
                return false;
            }
        } else if (expected.kind == HtmlTokenizerV1TokenKind::EndTag) {
            if (!require(
                    actual.name == expected.value && actual.data.empty() &&
                        actual.attributes.empty() && !actual.self_closing,
                    std::string(description) + " end tag at index " +
                        std::to_string(index))) {
                return false;
            }
        }
    }

    for (std::size_t index = 0U; index < expected_errors.size(); ++index) {
        const HtmlTokenizerV1ParseError& actual = sink.errors[index];
        const ExpectedError& expected = expected_errors[index];
        if (!require(
                actual.code == expected.code &&
                    actual.line == expected.line &&
                    actual.column == expected.column,
                std::string(description) + " parse error at index " +
                    std::to_string(index))) {
            return false;
        }
    }
    return true;
}

ExpectedToken character(std::string value) {
    return ExpectedToken{HtmlTokenizerV1TokenKind::Character, std::move(value)};
}

ExpectedToken end_tag(std::string value) {
    return ExpectedToken{HtmlTokenizerV1TokenKind::EndTag, std::move(value)};
}

bool run_text_state_pair(
    std::string_view description,
    std::string_view input,
    const std::vector<ExpectedToken>& tokens,
    const std::vector<ExpectedError>& errors = {}) {
    return run_case(
               description,
               HtmlTokenizerV1InitialState::Rcdata,
               "xmp",
               input,
               tokens,
               errors) &&
        run_case(
               description,
               HtmlTokenizerV1InitialState::Rawtext,
               "xmp",
               input,
               tokens,
               errors);
}

bool test_pinned_content_model_flag_semantics() {
    std::uint64_t executions = 0U;
    auto counted = [&executions](bool value, std::uint64_t count) {
        executions += count;
        return value;
    };

    if (!counted(
            run_case(
                "PLAINTEXT content model flag",
                HtmlTokenizerV1InitialState::Plaintext,
                "plaintext",
                "<head>&body;",
                {character("<head>&body;")},
                {}),
            1U) ||
        !counted(
            run_case(
                "PLAINTEXT with seeming close tag",
                HtmlTokenizerV1InitialState::Plaintext,
                "plaintext",
                "</plaintext>&body;",
                {character("</plaintext>&body;")},
                {}),
            1U) ||
        !counted(
            run_text_state_pair(
                "appropriate end tag",
                "foo</xmp>",
                {character("foo"), end_tag("xmp")}),
            2U) ||
        !counted(
            run_text_state_pair(
                "appropriate end tag case-insensitivity",
                "foo</xMp>",
                {character("foo"), end_tag("xmp")}),
            2U) ||
        !counted(
            run_text_state_pair(
                "end tag ending with space",
                "foo</xmp ",
                {character("foo")},
                {ExpectedError{"eof-in-tag", 1U, 10U}}),
            2U) ||
        !counted(
            run_text_state_pair(
                "end tag ending with EOF",
                "foo</xmp",
                {character("foo</xmp")}),
            2U) ||
        !counted(
            run_text_state_pair(
                "end tag ending with slash",
                "foo</xmp/",
                {character("foo")},
                {ExpectedError{"eof-in-tag", 1U, 10U}}),
            2U) ||
        !counted(
            run_text_state_pair(
                "end tag ending with left-angle-bracket",
                "foo</xmp<",
                {character("foo</xmp<")}),
            2U) ||
        !counted(
            run_text_state_pair(
                "incorrect end tag name",
                "</foo>bar</xmp>",
                {character("</foo>bar"), end_tag("xmp")}),
            2U) ||
        !counted(
            run_text_state_pair(
                "partial end tags reconsume",
                "</xmp</xmp</xmp>",
                {character("</xmp</xmp"), end_tag("xmp")}),
            2U) ||
        !counted(
            run_text_state_pair(
                "incorrect name starting with appropriate name",
                "</foo>bar</xmpaar>",
                {character("</foo>bar</xmpaar>")}),
            2U) ||
        !counted(
            run_text_state_pair(
                "appropriate close switches to Data end-tag subset",
                "foo</xmp></baz>",
                {character("foo"), end_tag("xmp"), end_tag("baz")}),
            2U) ||
        !counted(
            run_case(
                "RAWTEXT entity-looking bytes remain text",
                HtmlTokenizerV1InitialState::Rawtext,
                "xmp",
                "&foo;",
                {character("&foo;")},
                {}),
            1U) ||
        !counted(
            run_case(
                "RCDATA admitted entity is decoded",
                HtmlTokenizerV1InitialState::Rcdata,
                "textarea",
                "&lt;",
                {character("<")},
                {}),
            1U)) {
        return false;
    }
    return require(
        executions == 24U,
        "pinned contentModelFlags authority expands to exactly 24 executions");
}

bool test_character_token_bound_fails_closed() {
    CollectingSink sink;
    HtmlTokenizerV1Config config;
    config.maximum_token_bytes = 3U;
    HtmlTokenizerV1Stats stats;
    std::string error;
    return require(
               !tokenize_html_token_stream_v1(
                   "abcd",
                   HtmlTokenizerV1InitialState::Plaintext,
                   "",
                   config,
                   &sink,
                   &stats,
                   &error),
               "character token hard cap rejects oversized token") &&
        require(
            error.find("coalesced character token exceeds bounded byte limit") !=
                std::string::npos,
            "hard-cap failure is explicit") &&
        require(sink.tokens.empty(), "hard-cap failure emits no partial character token");
}

bool test_nul_preprocessing_gap_fails_closed() {
    CollectingSink sink;
    const std::string input("a\0b", 3U);
    std::string error;
    return require(
               !tokenize_html_token_stream_v1(
                   input,
                   HtmlTokenizerV1InitialState::Plaintext,
                   "plaintext",
                   {},
                   &sink,
                   nullptr,
                   &error),
               "NUL fails closed until preprocessing/replacement is admitted") &&
        require(
            error.find("preprocessing/NUL replacement is not implemented") !=
                std::string::npos,
            "NUL failure identifies preprocessing debt") &&
        require(sink.tokens.empty(), "NUL failure does not flush partial token");
}

bool test_invalid_initial_state_fails_closed() {
    CollectingSink sink;
    HtmlTokenizerV1Stats stats;
    std::string error;
    const auto invalid_state =
        static_cast<HtmlTokenizerV1InitialState>(255U);
    return require(
               !tokenize_html_token_stream_v1(
                   "x",
                   invalid_state,
                   "x",
                   {},
                   &sink,
                   &stats,
                   &error),
               "invalid initial-state enum is rejected") &&
        require(
            error.find("initial state is invalid") != std::string::npos,
            "invalid initial-state failure is explicit") &&
        require(
            sink.tokens.empty() && sink.errors.empty(),
            "invalid initial-state failure emits no events");
}

bool test_text_state_requires_last_start_tag() {
    CollectingSink sink;
    std::string error;
    const bool rcdata_rejected = !tokenize_html_token_stream_v1(
        "text",
        HtmlTokenizerV1InitialState::Rcdata,
        "",
        {},
        &sink,
        nullptr,
        &error);
    const bool rcdata_error =
        error.find("requires non-empty last-start-tag") != std::string::npos;

    error.clear();
    const bool rawtext_rejected = !tokenize_html_token_stream_v1(
        "text",
        HtmlTokenizerV1InitialState::Rawtext,
        "",
        {},
        &sink,
        nullptr,
        &error);
    const bool rawtext_error =
        error.find("requires non-empty last-start-tag") != std::string::npos;

    return require(rcdata_rejected, "RCDATA without last-start-tag is rejected") &&
        require(rcdata_error, "RCDATA last-start-tag rejection is explicit") &&
        require(rawtext_rejected, "RAWTEXT without last-start-tag is rejected") &&
        require(rawtext_error, "RAWTEXT last-start-tag rejection is explicit") &&
        require(
            sink.tokens.empty() && sink.errors.empty(),
            "missing last-start-tag failures emit no events");
}

} // namespace

int main() {
    if (!test_pinned_content_model_flag_semantics() ||
        !test_character_token_bound_fails_closed() ||
        !test_nul_preprocessing_gap_fails_closed() ||
        !test_invalid_initial_state_fails_closed() ||
        !test_text_state_requires_last_start_tag()) {
        return 1;
    }
    return 0;
}
