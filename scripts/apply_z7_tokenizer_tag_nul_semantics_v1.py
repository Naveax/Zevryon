#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


path = ROOT / "src/html_tokenizer_data_tags_v1.cpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''constexpr std::size_t kMaximumConfiguredAttributes = 4096U;
''',
    '''constexpr std::size_t kMaximumConfiguredAttributes = 4096U;
constexpr std::string_view kReplacementCharacterUtf8 = "\\xEF\\xBF\\xBD";
''',
    "tag NUL replacement scalar constant",
)

# Bogus comments entered from <? and invalid end-tag-open bytes replace NUL.
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
                if (!emit_parse_error(scan, "unexpected-null-character") ||
                    !append_bounded_bytes(
                        &data,
                        kReplacementCharacterUtf8,
                        "HTML Data-tag tokenizer bogus comment token")) {
                    return false;
                }
                ++scan;
                continue;
            }
            if (!ascii_byte(character)) {
''',
    "bogus-comment NUL",
)

# Quoted attribute values.
text = replace_once(
    text,
    '''                if (character == '\\0') {
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");
                }
                if (!ascii_byte(character)) {
''',
    '''                if (character == '\\0') {
                    if (!emit_parse_error(*cursor, "unexpected-null-character") ||
                        !append_bounded_bytes(
                            value,
                            kReplacementCharacterUtf8,
                            "HTML Data-tag tokenizer attribute value")) {
                        return false;
                    }
                    ++*cursor;
                    continue;
                }
                if (!ascii_byte(character)) {
''',
    "quoted attribute-value NUL",
)

# Unquoted attribute values.
text = replace_once(
    text,
    '''            if (character == '\\0') {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");
            }
            if (!ascii_byte(character)) {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer non-ASCII attribute-value authority is not implemented");
            }
''',
    '''            if (character == '\\0') {
                if (!emit_parse_error(*cursor, "unexpected-null-character") ||
                    !append_bounded_bytes(
                        value,
                        kReplacementCharacterUtf8,
                        "HTML Data-tag tokenizer attribute value")) {
                    return false;
                }
                ++*cursor;
                continue;
            }
            if (!ascii_byte(character)) {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer non-ASCII attribute-value authority is not implemented");
            }
''',
    "unquoted attribute-value NUL",
)

# Attribute-name NUL becomes U+FFFD after any already-required whitespace error.
text = replace_once(
    text,
    '''                if (character == '\\0') {
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");
                }
                if (!ascii_byte(character)) {
                    // Keep the historical v3 census classification stable until
''',
    '''                if (character == '\\0') {
                    if (!emit_parse_error(*cursor, "unexpected-null-character") ||
                        !append_bounded_bytes(
                            &attribute.name,
                            kReplacementCharacterUtf8,
                            "HTML Data-tag tokenizer attribute name")) {
                        return false;
                    }
                    ++*cursor;
                    continue;
                }
                if (!ascii_byte(character)) {
                    // Keep the historical v3 census classification stable until
''',
    "attribute-name NUL",
)

# Tag-open NUL is an ordinary invalid tag-name starter: emit '<' and reconsume
# the same byte in Data, where the already-admitted raw-NUL rule applies.
text = replace_once(
    text,
    '''        if (!ascii_alpha(input_[probe])) {
            if (input_[probe] == '\\0') {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");
            }
            if (!ascii_byte(input_[probe])) {
''',
    '''        if (!ascii_alpha(input_[probe])) {
            if (!ascii_byte(input_[probe])) {
''',
    "tag-open NUL gate",
)

# NUL inside a tag name is U+FFFD.
text = replace_once(
    text,
    '''            if (character == '\\0') {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");
            }
            if (!ascii_byte(character)) {
                // Preserve the historical census bucket until Unicode tag-name
''',
    '''            if (character == '\\0') {
                if (!emit_parse_error(probe, "unexpected-null-character") ||
                    !append_bounded_bytes(
                        &name,
                        kReplacementCharacterUtf8,
                        "HTML Data-tag tokenizer tag name")) {
                    return false;
                }
                ++probe;
                continue;
            }
            if (!ascii_byte(character)) {
                // Preserve the historical census bucket until Unicode tag-name
''',
    "tag-name NUL",
)

# Self-closing-start-tag recovery must emit unexpected-solidus first, then let
# attribute parsing consume/rewrite NUL. Removing this early gate preserves that
# ordering without broadening non-ASCII admission.
text = replace_once(
    text,
    '''                if (reconsume_character == '\\0') {
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");
                }
                if (!ascii_byte(reconsume_character)) {
''',
    '''                if (!ascii_byte(reconsume_character)) {
''',
    "solidus NUL reconsume gate",
)
path.write_text(text, encoding="utf-8")


path = ROOT / "tests/html_tokenizer_data_tag_recovery_v1_tests.cpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''    {
        CollectingSink sink;
        const std::string input("<\\0>", 3U);
        std::string error;
        if (!require(
                !tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                "tag-open NUL remains fail closed") ||
            !require(
                error.find("preprocessing/NUL replacement") != std::string::npos,
                "tag-open NUL failure remains explicit") ||
            !require(sink.tokens.empty() && sink.errors.empty(),
                     "tag-open NUL publishes no recovery events")) {
            return false;
        }
    }
''',
    '''    {
        CollectingSink sink;
        const std::string input("<\\0>", 3U);
        const std::string expected("<\\0>", 3U);
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                std::string("tag-open NUL reconsume: ") + error) ||
            !require(
                sink.tokens.size() == 1U &&
                    sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                    sink.tokens[0].data == expected,
                "tag-open NUL preserves Data payload") ||
            !require(
                sink.errors.size() == 2U &&
                    sink.errors[0].code == "invalid-first-character-of-tag-name" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == 2U &&
                    sink.errors[1].code == "unexpected-null-character" &&
                    sink.errors[1].line == 1U && sink.errors[1].column == 2U,
                "tag-open NUL exact error order")) {
            return false;
        }
    }
''',
    "tag-open NUL focused regression",
)
path.write_text(text, encoding="utf-8")


path = ROOT / "scripts/z7_html5lib_tokenizer_full_corpus_census_v1.py"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
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
    '''    # Diagnostic admission: ordinary Data NUL plus tag-family NUL that
    # enters through '<' but not markup declarations. Markup/DOCTYPE NUL stays
    # fail closed here.
    if "\\x00" in input_text:
        if state_name in {
            "PLAINTEXT state",
            "RCDATA state",
            "RAWTEXT state",
            "Script data state",
            "CDATA section state",
        }:
            pass
        elif state_name == "Data state" and (
            "<" not in input_text or
            (input_text.startswith("<") and not input_text.startswith("<!"))
        ):
            pass
        else:
            return "input-preprocessing-nul"
''',
    "tag NUL diagnostic classifier",
)
path.write_text(text, encoding="utf-8")

print("applied tag-family NUL semantics diagnostic patch")
