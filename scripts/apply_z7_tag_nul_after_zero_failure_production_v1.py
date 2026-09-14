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
    '''constexpr std::size_t kMaximumConfiguredAttributes = 4096U;\n''',
    '''constexpr std::size_t kMaximumConfiguredAttributes = 4096U;\nconstexpr std::string_view kReplacementCharacterUtf8 = "\\xEF\\xBF\\xBD";\n''',
    "replacement scalar constant",
)

text = replace_once(
    text,
    '''            if (character == '\\0') {\n                return fail_data_tokenizer(\n                    error_,\n                    "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");\n            }\n            if (!ascii_byte(character)) {\n                const std::size_t scalar_bytes =\n                    detail::html_tokenizer_utf8_scalar_bytes_v1(input_, scan);\n''',
    '''            if (character == '\\0') {\n                if (!emit_parse_error(scan, "unexpected-null-character") ||\n                    !append_bounded_bytes(\n                        &data,\n                        kReplacementCharacterUtf8,\n                        "HTML Data-tag tokenizer bogus comment token")) {\n                    return false;\n                }\n                ++scan;\n                continue;\n            }\n            if (!ascii_byte(character)) {\n                const std::size_t scalar_bytes =\n                    detail::html_tokenizer_utf8_scalar_bytes_v1(input_, scan);\n''',
    "bogus-comment NUL",
)

text = replace_once(
    text,
    '''                if (character == '\\0') {\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");\n                }\n                if (!ascii_byte(character)) {\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer non-ASCII attribute-value authority is not implemented");\n                }\n                if (ascii_control_parse_error(character) &&\n''',
    '''                if (character == '\\0') {\n                    if (!emit_parse_error(*cursor, "unexpected-null-character") ||\n                        !append_bounded_bytes(\n                            value,\n                            kReplacementCharacterUtf8,\n                            "HTML Data-tag tokenizer attribute value")) {\n                        return false;\n                    }\n                    ++*cursor;\n                    continue;\n                }\n                if (!ascii_byte(character)) {\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer non-ASCII attribute-value authority is not implemented");\n                }\n                if (ascii_control_parse_error(character) &&\n''',
    "quoted attribute-value NUL",
)

text = replace_once(
    text,
    '''            if (character == '\\0') {\n                return fail_data_tokenizer(\n                    error_,\n                    "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");\n            }\n            if (!ascii_byte(character)) {\n                return fail_data_tokenizer(\n                    error_,\n                    "HTML Data-tag tokenizer non-ASCII attribute-value authority is not implemented");\n            }\n            if (ascii_control_parse_error(character) &&\n''',
    '''            if (character == '\\0') {\n                if (!emit_parse_error(*cursor, "unexpected-null-character") ||\n                    !append_bounded_bytes(\n                        value,\n                        kReplacementCharacterUtf8,\n                        "HTML Data-tag tokenizer attribute value")) {\n                    return false;\n                }\n                ++*cursor;\n                continue;\n            }\n            if (!ascii_byte(character)) {\n                return fail_data_tokenizer(\n                    error_,\n                    "HTML Data-tag tokenizer non-ASCII attribute-value authority is not implemented");\n            }\n            if (ascii_control_parse_error(character) &&\n''',
    "unquoted attribute-value NUL",
)

text = replace_once(
    text,
    '''                if (character == '\\0') {\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");\n                }\n                if (!ascii_byte(character)) {\n                    // Keep the historical v3 census classification stable until\n''',
    '''                if (character == '\\0') {\n                    if (!emit_parse_error(*cursor, "unexpected-null-character") ||\n                        !append_bounded_bytes(\n                            &attribute.name,\n                            kReplacementCharacterUtf8,\n                            "HTML Data-tag tokenizer attribute name")) {\n                        return false;\n                    }\n                    ++*cursor;\n                    continue;\n                }\n                if (!ascii_byte(character)) {\n                    // Keep the historical v3 census classification stable until\n''',
    "attribute-name NUL",
)

text = replace_once(
    text,
    '''        if (!ascii_alpha(input_[probe])) {\n            if (input_[probe] == '\\0') {\n                return fail_data_tokenizer(\n                    error_,\n                    "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");\n            }\n            if (!ascii_byte(input_[probe])) {\n''',
    '''        if (!ascii_alpha(input_[probe])) {\n            if (!ascii_byte(input_[probe])) {\n''',
    "tag-open NUL gate",
)

text = replace_once(
    text,
    '''            if (character == '\\0') {\n                return fail_data_tokenizer(\n                    error_,\n                    "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");\n            }\n            if (!ascii_byte(character)) {\n                // Preserve the historical census bucket until Unicode tag-name\n''',
    '''            if (character == '\\0') {\n                if (!emit_parse_error(probe, "unexpected-null-character") ||\n                    !append_bounded_bytes(\n                        &name,\n                        kReplacementCharacterUtf8,\n                        "HTML Data-tag tokenizer tag name")) {\n                    return false;\n                }\n                ++probe;\n                continue;\n            }\n            if (!ascii_byte(character)) {\n                // Preserve the historical census bucket until Unicode tag-name\n''',
    "tag-name NUL",
)

text = replace_once(
    text,
    '''                if (reconsume_character == '\\0') {\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");\n                }\n                if (!ascii_byte(reconsume_character)) {\n''',
    '''                if (!ascii_byte(reconsume_character)) {\n''',
    "solidus NUL reconsume gate",
)
path.write_text(text, encoding="utf-8")

path = ROOT / "tests/html_tokenizer_data_tag_recovery_v1_tests.cpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''    {\n        CollectingSink sink;\n        const std::string input("<\\0>", 3U);\n        std::string error;\n        if (!require(\n                !tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),\n                "tag-open NUL remains fail closed") ||\n            !require(\n                error.find("preprocessing/NUL replacement") != std::string::npos,\n                "tag-open NUL failure remains explicit") ||\n            !require(sink.tokens.empty() && sink.errors.empty(),\n                     "tag-open NUL publishes no recovery events")) {\n            return false;\n        }\n    }\n''',
    '''    {\n        CollectingSink sink;\n        HtmlTokenizerDataTagsV1Stats stats;\n        const std::string input("<\\0>", 3U);\n        std::string error;\n        if (!require(\n                tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error),\n                std::string("tag-open NUL reconsume: ") + error) ||\n            !require(sink.tokens.size() == 1U &&\n                         sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&\n                         sink.tokens[0].data == input,\n                     "tag-open NUL preserves Data payload") ||\n            !require(sink.errors.size() == 2U &&\n                         sink.errors[0].code == "invalid-first-character-of-tag-name" &&\n                         sink.errors[0].line == 1U && sink.errors[0].column == 2U &&\n                         sink.errors[1].code == "unexpected-null-character" &&\n                         sink.errors[1].line == 1U && sink.errors[1].column == 2U,\n                     "tag-open NUL exact error order") ||\n            !require(stats.tokens_emitted == 1U &&\n                         stats.character_tokens_emitted == 1U &&\n                         stats.parse_errors_emitted == 2U,\n                     "tag-open NUL recovery stats")) {\n            return false;\n        }\n    }\n''',
    "focused tag-open NUL regression",
)
path.write_text(text, encoding="utf-8")

path = ROOT / "scripts/z7_html5lib_tokenizer_full_corpus_census_v1.py"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''        elif state_name == "Data state" and (\n            "<" not in input_text or\n            input_text[:9].lower() == "<!doctype" or\n            input_text.startswith("<!--")\n        ):\n            pass\n''',
    '''        elif state_name == "Data state" and (\n            "<" not in input_text or\n            input_text[:9].lower() == "<!doctype" or\n            input_text.startswith("<!--") or\n            (input_text.startswith("<") and not input_text.startswith("<!"))\n        ):\n            pass\n''',
    "tag-family NUL census admission",
)
text = replace_once(
    text,
    '''    require(\n        classify_pre_execution("tokenizer/test3.test", "Data state", "<a\\x00>", "", []) == "input-preprocessing-nul",\n        "markup Data NUL remains classified unsupported",\n    )\n''',
    '''    require(\n        classify_pre_execution("tokenizer/test3.test", "Data state", "<a\\x00>", "", []) is None,\n        "tag-family Data NUL is classified admitted",\n    )\n''',
    "tag-family NUL census self-test",
)
path.write_text(text, encoding="utf-8")

print("applied tag-state NUL production candidate after zero-failure fixes")
