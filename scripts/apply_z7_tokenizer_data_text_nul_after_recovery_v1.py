#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


# WHATWG Data state is intentionally different from RCDATA/RAWTEXT/Script:
# U+0000 emits unexpected-null-character and the current U+0000 itself. Select
# only DataTagTokenizer::run(); state-local tag/attribute/bogus-comment guards
# remain untouched.
path = ROOT / "src/html_tokenizer_data_tags_v1.cpp"
text = path.read_text(encoding="utf-8")
class_pos = text.index("class DataTagTokenizer final")
run_pos = text.index("    bool run() {", class_pos)
guard_pos = text.index("            if (character == '\\0') {", run_pos)
next_state_pos = text.index("            if (!ascii_byte(character)) {", guard_pos)
old_guard = text[guard_pos:next_state_pos]
expected_guard = '''            if (character == '\\0') {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");
            }
'''
if old_guard != expected_guard:
    raise SystemExit("ordinary Data-state raw NUL guard drifted")
new_guard = '''            if (character == '\\0') {
                if (!emit_parse_error(cursor, "unexpected-null-character") ||
                    !append_character(character)) {
                    return false;
                }
                ++cursor;
                continue;
            }
'''
text = text[:guard_pos] + new_guard + text[next_state_pos:]
path.write_text(text, encoding="utf-8")


path = ROOT / "tests/html_tokenizer_data_utf8_text_v1_tests.cpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''    {
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Stats stats;
        std::string error;
        const std::string input("A\\0B", 3U);
        if (!require(!tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error),
                     "NUL preprocessing remains outside slice") ||
            !require(error.find("NUL replacement is not implemented") != std::string::npos,
                     "NUL guard remains explicit")) {
            return false;
        }
    }
''',
    '''    {
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Stats stats;
        std::string error;
        const std::string input("A\\0B", 3U);
        const std::string expected("A\\0B", 3U);
        if (!require(tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error),
                     std::string("ordinary Data raw NUL: ") + error) ||
            !require(sink.tokens.size() == 1U &&
                         sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                         sink.tokens[0].data == expected,
                     "ordinary Data raw NUL payload") ||
            !require(sink.errors.size() == 1U &&
                         sink.errors[0].code == "unexpected-null-character" &&
                         sink.errors[0].line == 1U && sink.errors[0].column == 2U,
                     "ordinary Data raw NUL diagnostic") ||
            !require(stats.input_bytes == input.size() &&
                         stats.character_tokens_emitted == 1U &&
                         stats.character_bytes_emitted == expected.size() &&
                         stats.parse_errors_emitted == 1U,
                     "ordinary Data raw NUL stats")) {
            return false;
        }
    }
''',
    "Data UTF-8 raw NUL regression",
)
path.write_text(text, encoding="utf-8")


path = ROOT / "tests/html_tokenizer_data_tags_v1_tests.cpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''    {
        CollectingSink sink;
        const std::string input("a\\0b", 3U);
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
''',
    '''    {
        CollectingSink sink;
        const std::string input("a\\0b", 3U);
        const std::string expected("a\\0b", 3U);
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1(
                    input, {}, &sink, nullptr, &error),
                std::string("ordinary Data raw NUL: ") + error) ||
            !require(
                sink.tokens.size() == 1U &&
                    sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                    sink.tokens[0].data == expected,
                "ordinary Data raw NUL payload") ||
            !require(
                sink.errors.size() == 1U &&
                    sink.errors[0].code == "unexpected-null-character" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == 2U,
                "ordinary Data raw NUL exact diagnostic")) {
            return false;
        }
    }
''',
    "Data-tag raw NUL regression",
)
path.write_text(text, encoding="utf-8")


path = ROOT / "scripts/z7_html5lib_tokenizer_full_corpus_census_v1.py"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
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
    '''    # CDATA preserves raw NUL. PLAINTEXT/RCDATA/RAWTEXT and Script data
    # admit their state-specific behavior. Data admits only ordinary character
    # data here; any '<' keeps markup/comment/tag/DOCTYPE NUL fail closed.
    if "\\x00" in input_text:
        if state_name in {
            "PLAINTEXT state",
            "RCDATA state",
            "RAWTEXT state",
            "Script data state",
            "CDATA section state",
        }:
            pass
        elif state_name == "Data state" and "<" not in input_text:
            pass
        else:
            return "input-preprocessing-nul"
''',
    "bounded ordinary Data NUL classifier",
)
text = replace_once(
    text,
    '''    require(
        classify_pre_execution("tokenizer/test2.test", "Data state", "a\\x00b", "", []) == "input-preprocessing-nul",
        "NUL preprocessing classification",
    )
''',
    '''    require(
        classify_pre_execution("tokenizer/test2.test", "Data state", "a\\x00b", "", []) is None,
        "ordinary Data NUL admitted classification",
    )
    require(
        classify_pre_execution("tokenizer/test3.test", "Data state", "<a\\x00>", "", []) == "input-preprocessing-nul",
        "markup Data NUL remains classified unsupported",
    )
''',
    "Data NUL classifier self-test",
)
path.write_text(text, encoding="utf-8")

print("applied bounded ordinary Data-text raw NUL diagnostic patch")
