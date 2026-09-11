#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


cpp_path = ROOT / "src/html_tokenizer_token_stream_v1.cpp"
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
    '''        while (cursor < input_.size()) {
            if (input_[cursor] == '\\0') {
                return fail_tokenizer(
                    error_,
                    "HTML tokenizer input preprocessing/NUL replacement is not implemented in v1 token stream");
            }
            switch (state_) {
            case ActiveState::Plaintext:
                if (!append_character(input_[cursor])) {
                    return false;
                }
                ++cursor;
                break;
''',
    '''        while (cursor < input_.size()) {
            switch (state_) {
            case ActiveState::Plaintext:
                if (input_[cursor] == '\\0') {
                    if (!emit_parse_error(
                            cursor,
                            "unexpected-null-character") ||
                        !append_characters(kReplacementCharacterUtf8)) {
                        return false;
                    }
                    ++cursor;
                    break;
                }
                if (!append_character(input_[cursor])) {
                    return false;
                }
                ++cursor;
                break;
''',
    "state-local NUL dispatch",
)

cpp = replace_once(
    cpp,
    '''    bool consume_text_state(std::size_t* cursor, bool rcdata) {
        const char character = input_[*cursor];
        if (input_control_parse_error(character) &&
''',
    '''    bool consume_text_state(std::size_t* cursor, bool rcdata) {
        const char character = input_[*cursor];
        if (character == '\\0') {
            if (!emit_parse_error(
                    *cursor,
                    "unexpected-null-character") ||
                !append_characters(kReplacementCharacterUtf8)) {
                return false;
            }
            ++*cursor;
            return true;
        }
        if (input_control_parse_error(character) &&
''',
    "RCDATA/RAWTEXT NUL replacement",
)

cpp = replace_once(
    cpp,
    '''    bool consume_data_state(std::size_t* cursor) {
        const char character = input_[*cursor];
        if (character == '&') {
''',
    '''    bool consume_data_state(std::size_t* cursor) {
        const char character = input_[*cursor];
        if (character == '\\0') {
            return fail_tokenizer(
                error_,
                "HTML tokenizer post-text-state Data NUL handling is outside admitted v1 subset");
        }
        if (character == '&') {
''',
    "post-text Data NUL boundary",
)

cpp_path.write_text(cpp, encoding="utf-8")


tests_path = ROOT / "tests/html_tokenizer_token_stream_v1_tests.cpp"
tests = tests_path.read_text(encoding="utf-8")

old_test = r'''bool test_nul_preprocessing_gap_fails_closed() {
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
'''

new_test = r'''bool test_text_state_nul_replacement() {
    const std::string replacement("\xEF\xBF\xBD", 3U);
    const std::string plaintext_input("a\r\n\0b", 5U);
    return run_case(
               "PLAINTEXT CRLF plus NUL replacement",
               HtmlTokenizerV1InitialState::Plaintext,
               "plaintext",
               plaintext_input,
               {character(std::string("a\n") + replacement + "b")},
               {ExpectedError{"unexpected-null-character", 2U, 1U}}) &&
        run_case(
               "RCDATA NUL replacement",
               HtmlTokenizerV1InitialState::Rcdata,
               "xmp",
               std::string(1U, '\0'),
               {character(replacement)},
               {ExpectedError{"unexpected-null-character", 1U, 1U}}) &&
        run_case(
               "RAWTEXT NUL replacement",
               HtmlTokenizerV1InitialState::Rawtext,
               "xmp",
               std::string(1U, '\0'),
               {character(replacement)},
               {ExpectedError{"unexpected-null-character", 1U, 1U}});
}
'''
tests = replace_once(tests, old_test, new_test, "focused NUL test")

tests = replace_once(
    tests,
    '''        !test_crlf_input_preprocessing() ||
        !test_nul_preprocessing_gap_fails_closed() ||
''',
    '''        !test_crlf_input_preprocessing() ||
        !test_text_state_nul_replacement() ||
''',
    "focused NUL main registration",
)
tests_path.write_text(tests, encoding="utf-8")


census_path = ROOT / "scripts/z7_html5lib_tokenizer_full_corpus_census_v1.py"
census = census_path.read_text(encoding="utf-8")
census = replace_once(
    census,
    '''    # The html5lib CDATA initial-state authority intentionally preserves raw
    # NUL as Character data. Do not route that state through the generic
    # Data/text-state NUL preprocessing debt bucket.
    if "\\x00" in input_text and state_name != "CDATA section state":
        return "input-preprocessing-nul"
''',
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
    "text-state NUL census admission",
)
census_path.write_text(census, encoding="utf-8")


doc_path = ROOT / "docs/Z7_HTML_TOKENIZER_TOKEN_STREAM_V1.md"
doc = doc_path.read_text(encoding="utf-8")
doc = replace_once(
    doc,
    '''Raw NUL still fails closed because input-stream NUL replacement/preprocessing is not yet admitted.
''',
    '''Raw NUL is state-local rather than globally admitted. PLAINTEXT, RCDATA and RAWTEXT emit `unexpected-null-character` and append U+FFFD for U+0000. CDATA retains its separately admitted literal-NUL authority. Data and Script-data NUL behavior remain outside this slice and stay behind the corpus NUL authority boundary.
''',
    "NUL bounds documentation",
)
doc = replace_once(
    doc,
    '''Still outstanding are explicitly unsupported corpus surfaces such as CDATA initial-state handling, CR/NUL input-stream preprocessing, broader non-ASCII preprocessing/location authority, nullable DOCTYPE-name probe-wire representation and remaining malformed Data-tag/DOCTYPE/comment recovery buckets.
''',
    '''Still outstanding are explicitly unsupported corpus surfaces such as Data/Script-data NUL handling, broader non-ASCII preprocessing/location authority, nullable DOCTYPE-name probe-wire representation and remaining malformed Data-tag/DOCTYPE/comment recovery buckets.
''',
    "outstanding tokenizer debt documentation",
)
doc_path.write_text(doc, encoding="utf-8")

print("applied bounded PLAINTEXT/RCDATA/RAWTEXT NUL replacement diagnostic patch")
