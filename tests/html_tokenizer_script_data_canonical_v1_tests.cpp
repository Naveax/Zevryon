#include "html_tokenizer_token_stream_v1.hpp"

#include <cstddef>
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
        std::cerr << "FAILED: canonical Script-data v1: " << message << '\n';
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

bool character_is(const HtmlTokenizerV1Token& token, std::string_view data) {
    return token.kind == HtmlTokenizerV1TokenKind::Character &&
        token.data == data;
}

bool tag_is(
    const HtmlTokenizerV1Token& token,
    HtmlTokenizerV1TokenKind kind,
    std::string_view name) {
    return token.kind == kind && token.name == name;
}

bool test_script_data_initial_state_character_path() {
    const std::string input = "<!--<script>-</script>-->";
    CollectingSink sink;
    HtmlTokenizerV1Stats stats;
    std::string error;
    if (!require(
            tokenize_html_token_stream_v1(
                input,
                HtmlTokenizerV1InitialState::ScriptData,
                "",
                {},
                &sink,
                &stats,
                &error),
            std::string("Script-data character path: ") + error)) {
        return false;
    }
    return require(sink.tokens.size() == 1U, "character-path token count") &&
        require(character_is(sink.tokens[0], input), "character-path payload") &&
        require(sink.errors.empty(), "character-path parse errors") &&
        require(stats.input_bytes == input.size(), "character-path input bytes") &&
        require(stats.tokens_emitted == 1U &&
                    stats.character_tokens_emitted == 1U &&
                    stats.character_bytes_emitted == input.size() &&
                    stats.end_tags_emitted == 0U &&
                    stats.parse_errors_emitted == 0U,
                "character-path stats");
}

bool test_script_data_close_composes_data_suffix() {
    const std::string input = "x</ScRiPt><b>y</b>";
    CollectingSink sink;
    HtmlTokenizerV1Stats stats;
    std::string error;
    if (!require(
            tokenize_html_token_stream_v1(
                input,
                HtmlTokenizerV1InitialState::ScriptData,
                "SCRIPT",
                {},
                &sink,
                &stats,
                &error),
            std::string("Script-data/Data composition: ") + error) ||
        !require(sink.tokens.size() == 5U, "composition token count") ||
        !require(sink.errors.empty(), "composition parse errors")) {
        return false;
    }

    return require(character_is(sink.tokens[0], "x"), "composition leading character") &&
        require(tag_is(sink.tokens[1], HtmlTokenizerV1TokenKind::EndTag, "script"),
                "composition Script-data close") &&
        require(tag_is(sink.tokens[2], HtmlTokenizerV1TokenKind::StartTag, "b") &&
                    sink.tokens[2].attributes.empty(),
                "composition Data start tag") &&
        require(character_is(sink.tokens[3], "y"), "composition Data character") &&
        require(tag_is(sink.tokens[4], HtmlTokenizerV1TokenKind::EndTag, "b"),
                "composition Data end tag") &&
        require(stats.input_bytes == input.size(), "composition input bytes") &&
        require(stats.tokens_emitted == 5U &&
                    stats.character_tokens_emitted == 2U &&
                    stats.character_bytes_emitted == 2U &&
                    stats.end_tags_emitted == 2U &&
                    stats.parse_errors_emitted == 0U,
                "composition merged stats");
}

bool test_script_data_suffix_error_location_is_global() {
    const std::string input = "x\n</script><h a='b'c='d'>";
    CollectingSink sink;
    HtmlTokenizerV1Stats stats;
    std::string error;
    if (!require(
            tokenize_html_token_stream_v1(
                input,
                HtmlTokenizerV1InitialState::ScriptData,
                "script",
                {},
                &sink,
                &stats,
                &error),
            std::string("global suffix error: ") + error) ||
        !require(sink.tokens.size() == 3U, "global suffix token count") ||
        !require(sink.errors.size() == 1U, "global suffix error count")) {
        return false;
    }

    return require(character_is(sink.tokens[0], "x\n"), "global suffix leading character") &&
        require(tag_is(sink.tokens[1], HtmlTokenizerV1TokenKind::EndTag, "script"),
                "global suffix Script-data close") &&
        require(tag_is(sink.tokens[2], HtmlTokenizerV1TokenKind::StartTag, "h"),
                "global suffix Data start tag") &&
        require(sink.errors[0].code == "missing-whitespace-between-attributes" &&
                    sink.errors[0].line == 2U && sink.errors[0].column == 18U,
                "global suffix parse-error coordinates") &&
        require(stats.input_bytes == input.size(), "global suffix input bytes") &&
        require(stats.tokens_emitted == 3U &&
                    stats.character_tokens_emitted == 1U &&
                    stats.character_bytes_emitted == 2U &&
                    stats.end_tags_emitted == 1U &&
                    stats.parse_errors_emitted == 1U,
                "global suffix merged stats");
}

bool test_script_data_nul_fails_closed_through_canonical_entrypoint() {
    const std::string input("a\0b", 3U);
    CollectingSink sink;
    HtmlTokenizerV1Stats stats;
    std::string error;
    return require(
               !tokenize_html_token_stream_v1(
                   input,
                   HtmlTokenizerV1InitialState::ScriptData,
                   "script",
                   {},
                   &sink,
                   &stats,
                   &error),
               "canonical Script-data NUL remains fail closed") &&
        require(error.find("preprocessing/NUL replacement") != std::string::npos,
                "canonical Script-data NUL failure identifies preprocessing debt") &&
        require(sink.tokens.empty() && sink.errors.empty(),
                "canonical Script-data NUL failure publishes no partial events") &&
        require(stats.input_bytes == input.size() && stats.tokens_emitted == 0U,
                "canonical Script-data NUL failure stats");
}

} // namespace

int main() {
    if (!test_script_data_initial_state_character_path() ||
        !test_script_data_close_composes_data_suffix() ||
        !test_script_data_suffix_error_location_is_global() ||
        !test_script_data_nul_fails_closed_through_canonical_entrypoint()) {
        return 1;
    }
    return 0;
}
