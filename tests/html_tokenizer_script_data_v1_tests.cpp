#include "html_tokenizer_script_data_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using zevryon::massivedoc::HtmlTokenizerScriptDataV1Result;
using zevryon::massivedoc::HtmlTokenizerScriptDataV1Stats;
using zevryon::massivedoc::HtmlTokenizerV1Config;
using zevryon::massivedoc::HtmlTokenizerV1ParseError;
using zevryon::massivedoc::HtmlTokenizerV1Sink;
using zevryon::massivedoc::HtmlTokenizerV1Token;
using zevryon::massivedoc::HtmlTokenizerV1TokenKind;
using zevryon::massivedoc::consume_html_script_data_v1;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: HTML Script-data v1: " << message << '\n';
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

bool run_character_case(std::string_view description, std::string_view input) {
    CollectingSink sink;
    HtmlTokenizerScriptDataV1Stats stats;
    HtmlTokenizerScriptDataV1Result result;
    std::string error;
    if (!require(
            consume_html_script_data_v1(
                input,
                "",
                {},
                &sink,
                &stats,
                &result,
                &error),
            std::string(description) + ": " + error) ||
        !require(sink.tokens.size() == 1U, std::string(description) + " token count") ||
        !require(sink.errors.empty(), std::string(description) + " parse errors")) {
        return false;
    }
    return require(
               sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                   sink.tokens[0].data == input,
               std::string(description) + " character payload") &&
        require(!result.transitioned_to_data && result.next_offset == input.size(),
                std::string(description) + " EOF result") &&
        require(stats.input_bytes_available == input.size() &&
                    stats.bytes_consumed == input.size(),
                std::string(description) + " byte stats") &&
        require(stats.tokens_emitted == 1U &&
                    stats.character_tokens_emitted == 1U &&
                    stats.character_bytes_emitted == input.size() &&
                    stats.end_tags_emitted == 0U &&
                    stats.parse_errors_emitted == 0U,
                std::string(description) + " event stats");
}

bool test_pinned_test1_script_character_cases() {
    constexpr std::string_view inputs[] = {
        "<test-->",
        "<!test-->",
        "<!-test-->",
        "<!--test-->",
        "<!-- < test -->",
        "<!-- </ test -->",
        "<!-- <test> -->",
        "<!-- </test> -->",
        "<!--<script>-</script>-->",
        "<!--<script>--</script>-->",
        "<!--<script>---</script>-->",
        "<!--<script> - </script>-->",
        "<!--<script> -- </script>-->",
    };
    for (std::size_t index = 0U; index < std::size(inputs); ++index) {
        if (!run_character_case(
                std::string("pinned test1 Script-data case ") +
                    std::to_string(index),
                inputs[index])) {
            return false;
        }
    }
    return true;
}

bool test_appropriate_end_tag_transitions_to_data_boundary() {
    const std::string input = "x</ScRiPt>tail";
    CollectingSink sink;
    HtmlTokenizerScriptDataV1Stats stats;
    HtmlTokenizerScriptDataV1Result result;
    std::string error;
    if (!require(
            consume_html_script_data_v1(
                input,
                "SCRIPT",
                {},
                &sink,
                &stats,
                &result,
                &error),
            std::string("appropriate end tag: ") + error) ||
        !require(sink.tokens.size() == 2U, "appropriate close token count") ||
        !require(sink.errors.empty(), "appropriate close parse errors")) {
        return false;
    }
    const std::size_t tail_offset = input.find("tail");
    return require(
               sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                   sink.tokens[0].data == "x",
               "appropriate close leading character") &&
        require(sink.tokens[1].kind == HtmlTokenizerV1TokenKind::EndTag &&
                    sink.tokens[1].name == "script",
                "appropriate close end tag") &&
        require(result.transitioned_to_data && result.next_offset == tail_offset,
                "appropriate close transition boundary") &&
        require(stats.bytes_consumed == tail_offset &&
                    stats.tokens_emitted == 2U &&
                    stats.end_tags_emitted == 1U,
                "appropriate close stats");
}

bool test_appropriate_end_tag_with_whitespace_transitions() {
    const std::string input = "x</script   >tail";
    CollectingSink sink;
    HtmlTokenizerScriptDataV1Stats stats;
    HtmlTokenizerScriptDataV1Result result;
    std::string error;
    if (!require(
            consume_html_script_data_v1(
                input,
                "script",
                {},
                &sink,
                &stats,
                &result,
                &error),
            std::string("appropriate close with whitespace: ") + error) ||
        !require(sink.tokens.size() == 2U,
                 "appropriate close with whitespace token count") ||
        !require(sink.errors.empty(),
                 "appropriate close with whitespace parse errors")) {
        return false;
    }
    return require(
               sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                   sink.tokens[0].data == "x",
               "whitespace close leading character") &&
        require(sink.tokens[1].kind == HtmlTokenizerV1TokenKind::EndTag &&
                    sink.tokens[1].name == "script",
                "whitespace close end tag") &&
        require(result.transitioned_to_data &&
                    result.next_offset == input.find("tail"),
                "whitespace close transition boundary");
}

bool test_appropriate_end_tag_whitespace_eof_discards_tag() {
    const std::string input = "x</script ";
    CollectingSink sink;
    HtmlTokenizerScriptDataV1Stats stats;
    HtmlTokenizerScriptDataV1Result result;
    std::string error;
    if (!require(
            consume_html_script_data_v1(
                input,
                "script",
                {},
                &sink,
                &stats,
                &result,
                &error),
            std::string("appropriate close whitespace EOF: ") + error) ||
        !require(sink.tokens.size() == 1U,
                 "appropriate close whitespace EOF token count") ||
        !require(sink.errors.size() == 1U,
                 "appropriate close whitespace EOF error count")) {
        return false;
    }
    return require(
               sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                   sink.tokens[0].data == "x",
               "whitespace EOF only flushes preceding characters") &&
        require(sink.errors[0].code == "eof-in-tag" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == 11U,
                "whitespace EOF exact parse-error location") &&
        require(!result.transitioned_to_data && result.next_offset == input.size(),
                "whitespace EOF does not claim Data transition") &&
        require(stats.end_tags_emitted == 0U && stats.parse_errors_emitted == 1U,
                "whitespace EOF event stats");
}

bool test_nonappropriate_incomplete_end_tag_is_text() {
    const std::string input = "a</foo";
    CollectingSink sink;
    HtmlTokenizerScriptDataV1Stats stats;
    HtmlTokenizerScriptDataV1Result result;
    std::string error;
    if (!require(
            consume_html_script_data_v1(
                input,
                "script",
                {},
                &sink,
                &stats,
                &result,
                &error),
            std::string("nonappropriate incomplete end tag: ") + error) ||
        !require(sink.tokens.size() == 1U,
                 "nonappropriate incomplete token count") ||
        !require(sink.errors.empty(),
                 "nonappropriate incomplete parse errors")) {
        return false;
    }
    return require(
               sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                   sink.tokens[0].data == input,
               "nonappropriate incomplete spelling remains text") &&
        require(!result.transitioned_to_data && result.next_offset == input.size(),
                "nonappropriate incomplete spelling does not transition");
}

bool test_escaped_appropriate_end_tag_transitions() {
    const std::string input = "<!--x</script>tail";
    CollectingSink sink;
    HtmlTokenizerScriptDataV1Stats stats;
    HtmlTokenizerScriptDataV1Result result;
    std::string error;
    if (!require(
            consume_html_script_data_v1(
                input,
                "script",
                {},
                &sink,
                &stats,
                &result,
                &error),
            std::string("escaped appropriate close: ") + error) ||
        !require(sink.tokens.size() == 2U, "escaped appropriate close token count")) {
        return false;
    }
    return require(
               sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                   sink.tokens[0].data == "<!--x",
               "escaped close preceding text") &&
        require(sink.tokens[1].kind == HtmlTokenizerV1TokenKind::EndTag &&
                    sink.tokens[1].name == "script",
                "escaped close end tag") &&
        require(result.transitioned_to_data &&
                    result.next_offset == input.find("tail"),
                "escaped close transition boundary");
}

bool test_double_escaped_script_spelling_does_not_close() {
    const std::string input = "<!--<script>-</script>-->";
    CollectingSink sink;
    HtmlTokenizerScriptDataV1Stats stats;
    HtmlTokenizerScriptDataV1Result result;
    std::string error;
    if (!require(
            consume_html_script_data_v1(
                input,
                "script",
                {},
                &sink,
                &stats,
                &result,
                &error),
            std::string("double-escaped script spelling: ") + error) ||
        !require(sink.tokens.size() == 1U, "double-escaped token count")) {
        return false;
    }
    return require(
               sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                   sink.tokens[0].data == input,
               "double-escaped script spelling remains character data") &&
        require(!result.transitioned_to_data && result.next_offset == input.size(),
                "double-escaped spelling does not transition") &&
        require(stats.end_tags_emitted == 0U,
                "double-escaped spelling emits no end tag");
}

bool test_eof_in_comment_like_text_reports_error() {
    const std::string input = "<!--x";
    CollectingSink sink;
    HtmlTokenizerScriptDataV1Stats stats;
    HtmlTokenizerScriptDataV1Result result;
    std::string error;
    if (!require(
            consume_html_script_data_v1(
                input,
                "script",
                {},
                &sink,
                &stats,
                &result,
                &error),
            std::string("comment-like EOF: ") + error) ||
        !require(sink.tokens.size() == 1U, "comment-like EOF token count") ||
        !require(sink.errors.size() == 1U, "comment-like EOF error count")) {
        return false;
    }
    return require(
               sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                   sink.tokens[0].data == input,
               "comment-like EOF character payload") &&
        require(sink.errors[0].code == "eof-in-script-html-comment-like-text" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == 6U,
                "comment-like EOF error location") &&
        require(stats.parse_errors_emitted == 1U,
                "comment-like EOF error stats");
}

bool test_fail_closed_boundaries() {
    {
        const std::string input("a\0b", 3U);
        CollectingSink sink;
        HtmlTokenizerScriptDataV1Stats stats;
        HtmlTokenizerScriptDataV1Result result;
        std::string error;
        if (!require(
                !consume_html_script_data_v1(
                    input,
                    "script",
                    {},
                    &sink,
                    &stats,
                    &result,
                    &error),
                "NUL remains fail closed") ||
            !require(error.find("preprocessing/NUL replacement") != std::string::npos,
                     "NUL failure identifies preprocessing debt") ||
            !require(sink.tokens.empty() && sink.errors.empty(),
                     "NUL failure does not flush partial character data") ||
            !require(stats.bytes_consumed == 1U,
                     "NUL failure reports consumed prefix")) {
            return false;
        }
    }
    {
        const std::string input("a\xC3\xA9", 3U);
        CollectingSink sink;
        HtmlTokenizerScriptDataV1Stats stats;
        HtmlTokenizerScriptDataV1Result result;
        std::string error;
        if (!require(
                !consume_html_script_data_v1(
                    input,
                    "script",
                    {},
                    &sink,
                    &stats,
                    &result,
                    &error),
                "non-ASCII remains fail closed") ||
            !require(error.find("non-ASCII preprocessing/location authority") !=
                         std::string::npos,
                     "non-ASCII failure identifies location/preprocessing debt") ||
            !require(sink.tokens.empty() && sink.errors.empty(),
                     "non-ASCII failure does not flush partial character data") ||
            !require(stats.bytes_consumed == 1U,
                     "non-ASCII failure reports consumed ASCII prefix")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        HtmlTokenizerV1Config config;
        config.maximum_token_bytes = 3U;
        HtmlTokenizerScriptDataV1Stats stats;
        HtmlTokenizerScriptDataV1Result result;
        std::string error;
        if (!require(
                !consume_html_script_data_v1(
                    "abcd",
                    "",
                    config,
                    &sink,
                    &stats,
                    &result,
                    &error),
                "character token hard cap remains fail closed") ||
            !require(error.find("coalesced character token exceeds bounded byte limit") !=
                         std::string::npos,
                     "character hard-cap failure explicit") ||
            !require(sink.tokens.empty(), "character hard cap publishes no token")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        HtmlTokenizerScriptDataV1Stats stats;
        HtmlTokenizerScriptDataV1Result result;
        std::string error;
        if (!require(
                !consume_html_script_data_v1(
                    "</script/>",
                    "script",
                    {},
                    &sink,
                    &stats,
                    &result,
                    &error),
                "self-closing appropriate end tag remains fail closed") ||
            !require(error.find("self-closing appropriate end-tag recovery") !=
                         std::string::npos,
                     "self-closing end-tag failure explicit")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    if (!test_pinned_test1_script_character_cases() ||
        !test_appropriate_end_tag_transitions_to_data_boundary() ||
        !test_appropriate_end_tag_with_whitespace_transitions() ||
        !test_appropriate_end_tag_whitespace_eof_discards_tag() ||
        !test_nonappropriate_incomplete_end_tag_is_text() ||
        !test_escaped_appropriate_end_tag_transitions() ||
        !test_double_escaped_script_spelling_does_not_close() ||
        !test_eof_in_comment_like_text_reports_error() ||
        !test_fail_closed_boundaries()) {
        return 1;
    }
    return 0;
}
