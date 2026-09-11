#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


cpp_path = ROOT / "src/html_tokenizer_script_data_v1.cpp"
cpp = cpp_path.read_text(encoding="utf-8")

cpp = replace_once(
    cpp,
    '''constexpr std::size_t kMaximumConfiguredInputBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumConfiguredTokenBytes = 1024U * 1024U;
''',
    '''constexpr std::size_t kMaximumConfiguredInputBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumConfiguredTokenBytes = 1024U * 1024U;
constexpr std::string_view kReplacementCharacterUtf8 = "\\xEF\\xBF\\xBD";
''',
    "replacement scalar constant",
)

cpp = replace_once(
    cpp,
    '''        while (cursor < input_.size() && !done_) {
            if (input_[cursor] == '\\0') {
                stats_->bytes_consumed = static_cast<std::uint64_t>(cursor);
                return fail_script(
                    error_,
                    "HTML Script-data input preprocessing/NUL replacement is not implemented");
            }
            if (!ascii_byte(input_[cursor])) {
''',
    '''        while (cursor < input_.size() && !done_) {
            if (!ascii_byte(input_[cursor])) {
''',
    "remove global Script-data NUL rejection",
)

cpp = replace_once(
    cpp,
    '''private:
    bool append_character(char character) {
''',
    '''private:
    bool consume_null(std::size_t* cursor, ScriptState next_state) {
        if (!emit_parse_error(*cursor, "unexpected-null-character") ||
            !append_characters(kReplacementCharacterUtf8)) {
            return false;
        }
        state_ = next_state;
        ++*cursor;
        return true;
    }

    bool append_character(char character) {
''',
    "state-local NUL helper",
)

cpp = replace_once(
    cpp,
    '''        case ScriptState::Data:
            if (character == '<') {
''',
    '''        case ScriptState::Data:
            if (character == '\\0') {
                return consume_null(cursor, ScriptState::Data);
            }
            if (character == '<') {
''',
    "Script-data Data NUL",
)

cpp = replace_once(
    cpp,
    '''        case ScriptState::Escaped:
            if (character == '-') {
''',
    '''        case ScriptState::Escaped:
            if (character == '\\0') {
                return consume_null(cursor, ScriptState::Escaped);
            }
            if (character == '-') {
''',
    "Script-data Escaped NUL",
)

cpp = replace_once(
    cpp,
    '''        case ScriptState::EscapedDash:
            if (character == '-') {
''',
    '''        case ScriptState::EscapedDash:
            if (character == '\\0') {
                return consume_null(cursor, ScriptState::Escaped);
            }
            if (character == '-') {
''',
    "Script-data EscapedDash NUL",
)

cpp = replace_once(
    cpp,
    '''        case ScriptState::EscapedDashDash:
            if (character == '-') {
''',
    '''        case ScriptState::EscapedDashDash:
            if (character == '\\0') {
                return consume_null(cursor, ScriptState::Escaped);
            }
            if (character == '-') {
''',
    "Script-data EscapedDashDash NUL",
)

cpp = replace_once(
    cpp,
    '''        case ScriptState::DoubleEscaped:
            if (character == '-') {
''',
    '''        case ScriptState::DoubleEscaped:
            if (character == '\\0') {
                return consume_null(cursor, ScriptState::DoubleEscaped);
            }
            if (character == '-') {
''',
    "Script-data DoubleEscaped NUL",
)

cpp = replace_once(
    cpp,
    '''        case ScriptState::DoubleEscapedDash:
            if (character == '-') {
''',
    '''        case ScriptState::DoubleEscapedDash:
            if (character == '\\0') {
                return consume_null(cursor, ScriptState::DoubleEscaped);
            }
            if (character == '-') {
''',
    "Script-data DoubleEscapedDash NUL",
)

cpp = replace_once(
    cpp,
    '''        case ScriptState::DoubleEscapedDashDash:
            if (character == '-') {
''',
    '''        case ScriptState::DoubleEscapedDashDash:
            if (character == '\\0') {
                return consume_null(cursor, ScriptState::DoubleEscaped);
            }
            if (character == '-') {
''',
    "Script-data DoubleEscapedDashDash NUL",
)

cpp_path.write_text(cpp, encoding="utf-8")


header_path = ROOT / "src/html_tokenizer_script_data_v1.hpp"
header = header_path.read_text(encoding="utf-8")
header = replace_once(
    header,
    '''// The component implements the admitted ASCII Script-data escaped and
// double-escaped state family through the shared HtmlTokenizerV1Sink event
// boundary. NUL replacement/input preprocessing and unsupported appropriate
// end-tag attribute/self-closing recovery remain fail closed.
''',
    '''// The component implements the admitted ASCII Script-data escaped and
// double-escaped state family through the shared HtmlTokenizerV1Sink event
// boundary. Script-data U+0000 emits unexpected-null-character and U+FFFD in
// the states that consume Character data. Broader preprocessing and unsupported
// appropriate end-tag attribute/self-closing recovery remain fail closed.
''',
    "Script-data header contract",
)
header_path.write_text(header, encoding="utf-8")


tests_path = ROOT / "tests/html_tokenizer_script_data_v1_tests.cpp"
tests = tests_path.read_text(encoding="utf-8")

nul_tests = r'''bool run_nul_replacement_case(
    std::string_view description,
    const std::string& input) {
    const std::string replacement("\xEF\xBF\xBD", 3U);
    const std::size_t nul_offset = input.find('\0');
    if (!require(nul_offset != std::string::npos,
                 std::string(description) + " contains NUL")) {
        return false;
    }

    std::string expected = input;
    expected.replace(nul_offset, 1U, replacement);

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
            std::string(description) + ": " + error) ||
        !require(sink.tokens.size() == 1U,
                 std::string(description) + " token count") ||
        !require(sink.errors.size() == 1U,
                 std::string(description) + " parse-error count")) {
        return false;
    }

    return require(
               sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                   sink.tokens[0].data == expected,
               std::string(description) + " replacement payload") &&
        require(
            sink.errors[0].code == "unexpected-null-character" &&
                sink.errors[0].line == 1U &&
                sink.errors[0].column == nul_offset + 1U,
            std::string(description) + " exact parse-error location") &&
        require(
            !result.transitioned_to_data && result.next_offset == input.size(),
            std::string(description) + " EOF result") &&
        require(
            stats.input_bytes_available == input.size() &&
                stats.bytes_consumed == input.size() &&
                stats.tokens_emitted == 1U &&
                stats.character_tokens_emitted == 1U &&
                stats.character_bytes_emitted == input.size() + 2U &&
                stats.end_tags_emitted == 0U &&
                stats.parse_errors_emitted == 1U,
            std::string(description) + " event stats");
}

bool test_nul_replacement_across_script_character_states() {
    const std::string cases[] = {
        std::string("a\0b", 3U),
        std::string("<!--x\0-->", 10U),
        std::string("<!--x-\0-->", 11U),
        std::string("<!--\0-->", 9U),
        std::string("<!--<script>\0</script>-->", 26U),
        std::string("<!--<script>x-\0</script>-->", 28U),
        std::string("<!--<script>--\0</script>-->", 29U),
    };
    for (std::size_t index = 0U; index < std::size(cases); ++index) {
        if (!run_nul_replacement_case(
                std::string("Script-data NUL state case ") +
                    std::to_string(index),
                cases[index])) {
            return false;
        }
    }
    return true;
}

'''

tests = replace_once(
    tests,
    '''bool test_fail_closed_boundaries() {
    {
        const std::string input("a\\0b", 3U);
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
''',
    nul_tests + '''bool test_fail_closed_boundaries() {
''',
    "replace Script-data NUL fail-closed regression",
)

tests = replace_once(
    tests,
    '''        !test_eof_in_comment_like_text_reports_error() ||
        !test_fail_closed_boundaries()) {
''',
    '''        !test_eof_in_comment_like_text_reports_error() ||
        !test_nul_replacement_across_script_character_states() ||
        !test_fail_closed_boundaries()) {
''',
    "register Script-data NUL regressions",
)

tests_path.write_text(tests, encoding="utf-8")


census_path = ROOT / "scripts/z7_html5lib_tokenizer_full_corpus_census_v1.py"
census = census_path.read_text(encoding="utf-8")
census = replace_once(
    census,
    '''    # CDATA preserves raw NUL. PLAINTEXT/RCDATA/RAWTEXT now admit their
    # tokenizer-state U+FFFD replacement + unexpected-null-character behavior.
    # Data and Script data remain behind the separate NUL authority boundary.
    if "\\x00" in input_text and state_name not in {
        "PLAINTEXT state",
        "RCDATA state",
        "RAWTEXT state",
        "CDATA section state",
    }:
        return "input-preprocessing-nul"
''',
    '''    # CDATA preserves raw NUL. PLAINTEXT/RCDATA/RAWTEXT and Script data
    # now admit their state-specific U+FFFD replacement behavior. Data remains
    # behind the separate NUL authority boundary because its markup/comment/
    # DOCTYPE state family needs a distinct bounded admission slice.
    if "\\x00" in input_text and state_name not in {
        "PLAINTEXT state",
        "RCDATA state",
        "RAWTEXT state",
        "Script data state",
        "CDATA section state",
    }:
        return "input-preprocessing-nul"
''',
    "Script-data NUL census admission",
)
census_path.write_text(census, encoding="utf-8")


doc_path = ROOT / "docs/Z7_HTML_TOKENIZER_SCRIPT_DATA_V1.md"
doc = doc_path.read_text(encoding="utf-8")
doc = replace_once(
    doc,
    '''- NUL/preprocessing debt remains fail-closed through the canonical entrypoint
  without publishing a partial Character token.
''',
    '''- Script-data U+0000 is handled state-locally across ordinary, escaped and
  double-escaped Character-consuming states: one `unexpected-null-character`
  is emitted and UTF-8 U+FFFD is appended without disturbing state recovery.
''',
    "Script-data regression documentation",
)
doc = replace_once(
    doc,
    '''- U+0000 replacement or complete input-stream preprocessing;
- general non-ASCII preprocessing/location authority;
''',
    '''- Data/markup/comment/DOCTYPE U+0000 recovery or complete input-stream preprocessing;
- general non-ASCII preprocessing/location authority;
''',
    "Script-data fail-closed documentation",
)
doc = replace_once(
    doc,
    '''Complete character references, preprocessing, CDATA, broader recovery, full
`test1.test` execution, tree-builder-driven tokenizer transitions and
`tree_builder_conformance` remain outstanding. Z7 remains `planned`.
''',
    '''Complete Data-state NUL recovery, broader preprocessing/non-ASCII authority,
character-reference and malformed-markup recovery gaps, full tokenizer corpus
conformance, tree-builder-driven tokenizer transitions and
`tree_builder_conformance` remain outstanding. Z7 remains `planned`.
''',
    "Script-data conformance status documentation",
)
doc_path.write_text(doc, encoding="utf-8")

print("applied bounded Script-data state-local NUL replacement diagnostic patch")
