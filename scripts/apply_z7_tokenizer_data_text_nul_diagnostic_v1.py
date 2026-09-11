#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


# Production Data character state only. Markup/comment/tag/DOCTYPE NUL sites remain
# explicitly fail closed in their existing state-local code paths.
path = ROOT / "src/html_tokenizer_data_tags_v1.cpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''constexpr std::size_t kMaximumConfiguredInputBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumConfiguredTokenBytes = 1024U * 1024U;
constexpr std::size_t kMaximumConfiguredAttributes = 4096U;
''',
    '''constexpr std::size_t kMaximumConfiguredInputBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumConfiguredTokenBytes = 1024U * 1024U;
constexpr std::size_t kMaximumConfiguredAttributes = 4096U;
constexpr std::string_view kReplacementCharacterUtf8 = "\\xEF\\xBF\\xBD";
''',
    "Data replacement scalar constant",
)
text = replace_once(
    text,
    '''            if (character == '\\0') {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");
            }
            if (!ascii_byte(character)) {
''',
    '''            if (character == '\\0') {
                if (!emit_parse_error(cursor, "unexpected-null-character") ||
                    !append_bounded_bytes(
                        &character_buffer_,
                        kReplacementCharacterUtf8,
                        "HTML Data-tag tokenizer coalesced character token")) {
                    return false;
                }
                ++cursor;
                continue;
            }
            if (!ascii_byte(character)) {
''',
    "ordinary Data-state NUL replacement",
)
path.write_text(text, encoding="utf-8")


# Dedicated UTF-8/Data focused regression now freezes ordinary Data-state NUL.
path = ROOT / "tests/html_tokenizer_data_utf8_text_v1_tests.cpp"
text = path.read_text(encoding="utf-8")
old = '''    {
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
'''
new = '''    {
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Stats stats;
        std::string error;
        const std::string input("A\\0B", 3U);
        std::string expected = "A";
        expected.append("\\xEF\\xBF\\xBD", 3U);
        expected.push_back('B');
        if (!require(tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error),
                     std::string("ordinary Data NUL replacement: ") + error) ||
            !require(sink.tokens.size() == 1U &&
                         sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                         sink.tokens[0].data == expected,
                     "ordinary Data NUL replacement payload") ||
            !require(sink.errors.size() == 1U &&
                         sink.errors[0].code == "unexpected-null-character" &&
                         sink.errors[0].line == 1U && sink.errors[0].column == 2U,
                     "ordinary Data NUL diagnostic") ||
            !require(stats.input_bytes == input.size() &&
                         stats.character_tokens_emitted == 1U &&
                         stats.character_bytes_emitted == expected.size() &&
                         stats.parse_errors_emitted == 1U,
                     "ordinary Data NUL stats")) {
            return false;
        }
    }
'''
text = replace_once(text, old, new, "Data UTF-8 NUL regression")
path.write_text(text, encoding="utf-8")


# Core Data-tag regression mirrors the positive replacement contract.
path = ROOT / "tests/html_tokenizer_data_tags_v1_tests.cpp"
text = path.read_text(encoding="utf-8")
old = '''    {
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
'''
new = '''    {
        CollectingSink sink;
        const std::string input("a\\0b", 3U);
        std::string expected = "a";
        expected.append("\\xEF\\xBF\\xBD", 3U);
        expected.push_back('b');
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1(
                    input, {}, &sink, nullptr, &error),
                std::string("ordinary Data NUL replacement: ") + error) ||
            !require(
                sink.tokens.size() == 1U &&
                    sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                    sink.tokens[0].data == expected,
                "ordinary Data NUL replacement payload") ||
            !require(
                sink.errors.size() == 1U &&
                    sink.errors[0].code == "unexpected-null-character" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == 2U,
                "ordinary Data NUL exact diagnostic")) {
            return false;
        }
    }
'''
text = replace_once(text, old, new, "Data-tag NUL regression")
path.write_text(text, encoding="utf-8")


# Corpus admission stays intentionally narrower than all Data-state NUL. Only
# executions without '<' can remain in ordinary Data character state for the
# entire input; markup/comment/tag/DOCTYPE NUL stays preclassified unsupported.
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
    # admit their state-specific replacement behavior. Data now admits only the
    # ordinary Character-data path: any '<' keeps the execution behind the
    # markup/comment/tag/DOCTYPE NUL authority boundary.
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

print("applied bounded ordinary Data-text NUL diagnostic patch")
