#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


# Production: admit only the top-level Data state's U+0000 behavior. WHATWG Data
# emits unexpected-null-character and the current U+0000 itself; unlike
# RCDATA/RAWTEXT/Script-data it does not substitute U+FFFD here.
path = ROOT / "src/html_tokenizer_data_tags_v1.cpp"
text = path.read_text(encoding="utf-8")
class_pos = text.index("class DataTagTokenizer final")
run_pos = text.index("    bool run() {", class_pos)
guard_pos = text.index("            if (character == '\\0') {", run_pos)
next_state_pos = text.index("            if (!ascii_byte(character)) {", guard_pos)
old_guard = text[guard_pos:next_state_pos]
if "input preprocessing/NUL replacement is not implemented" not in old_guard:
    raise SystemExit("ordinary Data-state NUL guard drifted")
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


# Focused UTF-8/Data regression: top-level NUL now succeeds while markup NUL
# remains covered by the separate recovery guard suite.
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
                     std::string("ordinary Data NUL: ") + error) ||
            !require(sink.tokens.size() == 1U &&
                         sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                         sink.tokens[0].data == expected,
                     "ordinary Data NUL preserves U+0000 payload") ||
            !require(sink.errors.size() == 1U &&
                         sink.errors[0].code == "unexpected-null-character" &&
                         sink.errors[0].line == 1U && sink.errors[0].column == 2U,
                     "ordinary Data NUL exact diagnostic") ||
            !require(stats.input_bytes == input.size() &&
                         stats.tokens_emitted == 1U &&
                         stats.character_tokens_emitted == 1U &&
                         stats.character_bytes_emitted == expected.size() &&
                         stats.parse_errors_emitted == 1U,
                     "ordinary Data NUL exact stats")) {
            return false;
        }
    }
''',
    "Data UTF-8 raw-NUL regression",
)
path.write_text(text, encoding="utf-8")


# Core Data-tag regression freezes the same semantics at the component boundary.
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
                std::string("ordinary Data NUL: ") + error) ||
            !require(
                sink.tokens.size() == 1U &&
                    sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                    sink.tokens[0].data == expected,
                "ordinary Data NUL preserves U+0000 payload") ||
            !require(
                sink.errors.size() == 1U &&
                    sink.errors[0].code == "unexpected-null-character" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == 2U,
                "ordinary Data NUL exact diagnostic")) {
            return false;
        }
    }
''',
    "Data-tag raw-NUL regression",
)
path.write_text(text, encoding="utf-8")


# Corpus admission remains bounded: only Data-state executions that never enter
# markup via '<' are promoted. Tag/comment/DOCTYPE/attribute NUL debt remains
# explicitly unsupported until each consuming state is separately admitted.
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
    # admit their existing state-specific behavior. Data now admits only the
    # ordinary character-data path: any '<' keeps markup/comment/tag/DOCTYPE NUL
    # behind its separate state-local authority boundary.
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
    "ordinary Data NUL classifier self-test",
)
path.write_text(text, encoding="utf-8")


# Documentation: make the narrow state distinction explicit instead of claiming
# all Data NUL remains fail-closed.
path = ROOT / "docs/Z7_HTML_TOKENIZER_DATA_TAGS_V1.md"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''- coalesced Character tokens outside tags;
''',
    '''- coalesced Character tokens outside tags;
- ordinary Data-state U+0000 as literal U+0000 Character data plus `unexpected-null-character`;
''',
    "Data-tags admitted raw NUL",
)
text = replace_once(
    text,
    '''- NUL replacement and complete input-stream preprocessing;
''',
    '''- NUL handling in tag, attribute, comment and DOCTYPE states; only ordinary Data character-state U+0000 is admitted here;
''',
    "Data-tags NUL boundary",
)
path.write_text(text, encoding="utf-8")

path = ROOT / "docs/Z7_HTML_TOKENIZER_DATA_UTF8_TEXT_V1.md"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''- Valid scalar bytes are preserved byte-for-byte in the coalesced Character token.
''',
    '''- Valid scalar bytes are preserved byte-for-byte in the coalesced Character token.
- Ordinary Data-state U+0000 emits `unexpected-null-character` and is preserved as literal U+0000 Character data.
''',
    "Data UTF-8 raw NUL admission",
)
text = replace_once(
    text,
    '''- raw U+0000 / NUL preprocessing;
''',
    '''- U+0000 handling after transitions into tag, attribute, comment or DOCTYPE states;
''',
    "Data UTF-8 NUL boundary",
)
text = replace_once(
    text,
    '''The dedicated `html-tokenizer-data-utf8-text-v1-tests` target freezes valid 2/3/4-byte passthrough, literal ampersand fallback, scalar-aware error columns, malformed UTF-8 rejection, token-byte bounds, and the retained non-ASCII markup/NUL guards.
''',
    '''The dedicated `html-tokenizer-data-utf8-text-v1-tests` target freezes valid 2/3/4-byte passthrough, literal ampersand fallback, scalar-aware error columns, malformed UTF-8 rejection, token-byte bounds, ordinary Data-state U+0000 authority, and the retained non-ASCII markup guards.
''',
    "Data UTF-8 verification wording",
)
path.write_text(text, encoding="utf-8")

path = ROOT / "docs/Z7_HTML_TOKENIZER_DATA_TAG_RECOVERY_V1.md"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''- U+0000 remains fail-closed until replacement/preprocessing semantics are admitted;
''',
    '''- U+0000 in tag-open/end-tag-open/tag/attribute recovery remains fail-closed; only the separate ordinary Data character-state path is admitted;
''',
    "Data tag recovery NUL boundary",
)
path.write_text(text, encoding="utf-8")

path = ROOT / "docs/Z7_HTML_TOKENIZER_TOKEN_STREAM_V1.md"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''Raw NUL is state-local rather than globally admitted. PLAINTEXT, RCDATA, RAWTEXT and Script-data emit `unexpected-null-character` and append U+FFFD for U+0000 in their admitted Character-consuming states. CDATA retains its separately admitted literal-NUL authority. Data/markup/comment/DOCTYPE NUL behavior remains behind the corpus NUL authority boundary.
''',
    '''Raw NUL is state-local rather than globally admitted. PLAINTEXT, RCDATA, RAWTEXT and Script-data emit `unexpected-null-character` and append U+FFFD for U+0000 in their admitted Character-consuming states. Ordinary Data character state instead emits `unexpected-null-character` and preserves the current U+0000 as Character data. CDATA retains its separately admitted literal-NUL authority. Tag/attribute/comment/DOCTYPE NUL behavior remains behind the corpus NUL authority boundary.
''',
    "token-stream raw NUL authority",
)
text = replace_once(
    text,
    '''- state-local NUL recovery plus remaining Data/markup preprocessing debt;
''',
    '''- state-local NUL recovery including ordinary Data character state, with tag/attribute/comment/DOCTYPE NUL debt retained;
''',
    "token-stream focused NUL coverage",
)
text = replace_once(
    text,
    '''Still outstanding are explicitly unsupported corpus surfaces such as Data/markup/comment/DOCTYPE NUL handling, broader non-ASCII preprocessing/location authority and remaining malformed Data-tag/DOCTYPE/comment recovery buckets. Nullable DOCTYPE-name probe-wire representation is admitted by this slice.
''',
    '''Still outstanding are explicitly unsupported corpus surfaces such as tag/attribute/comment/DOCTYPE NUL handling, broader non-ASCII preprocessing/location authority and remaining malformed Data-tag/DOCTYPE/comment recovery buckets. Ordinary Data character-state U+0000 and nullable DOCTYPE-name probe-wire representation are admitted.
''',
    "token-stream admission boundary",
)
path.write_text(text, encoding="utf-8")

print("applied bounded ordinary Data-text U+0000 production patch")
