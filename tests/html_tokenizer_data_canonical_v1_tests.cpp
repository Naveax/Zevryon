#include "html_tokenizer_token_stream_v1.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using zevryon::massivedoc::HtmlTokenizerV1InitialState;
using zevryon::massivedoc::HtmlTokenizerV1ParseError;
using zevryon::massivedoc::HtmlTokenizerV1Sink;
using zevryon::massivedoc::HtmlTokenizerV1Stats;
using zevryon::massivedoc::HtmlTokenizerV1Token;
using zevryon::massivedoc::HtmlTokenizerV1TokenKind;
using zevryon::massivedoc::tokenize_html_token_stream_v1;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: canonical HTML Data state v1: " << message << '\n';
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
    return token.kind == HtmlTokenizerV1TokenKind::Character &&
        token.data == data && token.name.empty();
}

bool token_is_tag(
    const HtmlTokenizerV1Token& token,
    HtmlTokenizerV1TokenKind kind,
    std::string_view name) {
    return token.kind == kind && token.name == name;
}

bool test_data_initial_state_preserves_full_admitted_token_order() {
    const std::string input =
        "a<!--c--><H x=y>z</h><!DOCTYPE html>q";
    CollectingSink sink;
    HtmlTokenizerV1Stats stats;
    std::string error;
    if (!require(
            tokenize_html_token_stream_v1(
                input,
                HtmlTokenizerV1InitialState::Data,
                "",
                {},
                &sink,
                &stats,
                &error),
            std::string("ordered Data composition: ") + error) ||
        !require(sink.tokens.size() == 7U, "ordered token count") ||
        !require(sink.errors.empty(), "ordered Data composition errors")) {
        return false;
    }

    return require(token_is_character(sink.tokens[0], "a"), "leading character") &&
        require(sink.tokens[1].kind == HtmlTokenizerV1TokenKind::Comment &&
                    sink.tokens[1].data == "c",
                "comment token") &&
        require(token_is_tag(sink.tokens[2], HtmlTokenizerV1TokenKind::StartTag, "h") &&
                    sink.tokens[2].attributes.size() == 1U &&
                    sink.tokens[2].attributes[0].name == "x" &&
                    sink.tokens[2].attributes[0].value == "y",
                "start tag and attribute") &&
        require(token_is_character(sink.tokens[3], "z"), "middle character") &&
        require(token_is_tag(sink.tokens[4], HtmlTokenizerV1TokenKind::EndTag, "h"),
                "end tag") &&
        require(sink.tokens[5].kind == HtmlTokenizerV1TokenKind::Doctype &&
                    sink.tokens[5].name == "html" &&
                    !sink.tokens[5].has_public_identifier &&
                    !sink.tokens[5].has_system_identifier &&
                    !sink.tokens[5].force_quirks,
                "DOCTYPE token") &&
        require(token_is_character(sink.tokens[6], "q"), "trailing character") &&
        require(stats.input_bytes == input.size(), "input-byte stats") &&
        require(stats.tokens_emitted == 7U, "aggregate token stats") &&
        require(stats.character_tokens_emitted == 3U &&
                    stats.character_bytes_emitted == 3U,
                "aggregate character stats") &&
        require(stats.end_tags_emitted == 1U, "aggregate end-tag stats") &&
        require(stats.parse_errors_emitted == 0U, "aggregate parse-error stats");
}

bool test_data_parse_error_coordinates_and_common_stats_are_preserved() {
    const std::string input = "<!--c--><h a='b'c='d'>";
    CollectingSink sink;
    HtmlTokenizerV1Stats stats;
    std::string error;
    if (!require(
            tokenize_html_token_stream_v1(
                input,
                HtmlTokenizerV1InitialState::Data,
                "",
                {},
                &sink,
                &stats,
                &error),
            std::string("recoverable Data parse error: ") + error) ||
        !require(sink.tokens.size() == 2U, "recoverable Data token count") ||
        !require(sink.errors.size() == 1U, "recoverable Data error count")) {
        return false;
    }

    return require(sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Comment &&
                    sink.tokens[0].data == "c",
                "recoverable Data comment") &&
        require(token_is_tag(sink.tokens[1], HtmlTokenizerV1TokenKind::StartTag, "h"),
                "recoverable Data start tag") &&
        require(sink.errors[0].code == "missing-whitespace-between-attributes" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == 17U,
                "global parse-error coordinates") &&
        require(stats.input_bytes == input.size(), "recoverable Data input stats") &&
        require(stats.tokens_emitted == 2U, "recoverable Data token stats") &&
        require(stats.parse_errors_emitted == 1U, "recoverable Data error stats");
}

bool test_data_failure_retains_already_accepted_events_and_stats() {
    const std::string input = "<!--ok--><!DOCTYPE html PUBLIC 'x'>";
    CollectingSink sink;
    HtmlTokenizerV1Stats stats;
    std::string error;
    if (!require(
            !tokenize_html_token_stream_v1(
                input,
                HtmlTokenizerV1InitialState::Data,
                "",
                {},
                &sink,
                &stats,
                &error),
            "unsupported PUBLIC doctype fails closed") ||
        !require(error.find("PUBLIC/SYSTEM") != std::string::npos,
                 "PUBLIC doctype failure is explicit")) {
        return false;
    }

    return require(sink.tokens.size() == 1U, "accepted prefix event is retained") &&
        require(sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Comment &&
                    sink.tokens[0].data == "ok",
                "accepted prefix comment is retained") &&
        require(stats.input_bytes == input.size(), "failure retains full input stats") &&
        require(stats.tokens_emitted == 1U,
                "failure retains accepted prefix token accounting") &&
        require(stats.character_tokens_emitted == 0U &&
                    stats.character_bytes_emitted == 0U &&
                    stats.end_tags_emitted == 0U,
                "failure common counters do not invent events");
}

bool test_data_initial_state_does_not_validate_unused_last_start_tag() {
    CollectingSink sink;
    HtmlTokenizerV1Stats stats;
    std::string error;
    if (!require(
            tokenize_html_token_stream_v1(
                "x",
                HtmlTokenizerV1InitialState::Data,
                "unused!invalid!tag",
                {},
                &sink,
                &stats,
                &error),
            std::string("Data ignores last-start-tag: ") + error)) {
        return false;
    }

    return require(sink.tokens.size() == 1U &&
                    token_is_character(sink.tokens[0], "x"),
                "Data emits character with unused last-start-tag") &&
        require(sink.errors.empty(), "Data unused last-start-tag adds no error") &&
        require(stats.tokens_emitted == 1U &&
                    stats.character_tokens_emitted == 1U &&
                    stats.character_bytes_emitted == 1U,
                "Data unused last-start-tag stats");
}

} // namespace

int main() {
    if (!test_data_initial_state_preserves_full_admitted_token_order() ||
        !test_data_parse_error_coordinates_and_common_stats_are_preserved() ||
        !test_data_failure_retains_already_accepted_events_and_stats() ||
        !test_data_initial_state_does_not_validate_unused_last_start_tag()) {
        return 1;
    }
    return 0;
}
