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
    '''bool unicode_noncharacter(std::uint32_t scalar) noexcept {\n    return (scalar >= 0xFDD0U && scalar <= 0xFDEFU) ||\n        (scalar <= 0x10FFFFU && (scalar & 0xFFFFU) >= 0xFFFEU);\n}\n\n''',
    '''bool unicode_noncharacter(std::uint32_t scalar) noexcept {\n    return (scalar >= 0xFDD0U && scalar <= 0xFDEFU) ||\n        (scalar <= 0x10FFFFU && (scalar & 0xFFFFU) >= 0xFFFEU);\n}\n\nbool unicode_control_parse_error(std::uint32_t scalar) noexcept {\n    return scalar >= 0x80U && scalar <= 0x9FU;\n}\n\n''',
    "Unicode control helper",
)

text = replace_once(
    text,
    '''            if (!ascii_byte(input_[probe])) {\n                return fail_data_tokenizer(\n                    error_,\n                    "HTML Data-tag tokenizer non-ASCII preprocessing/location authority is not implemented");\n            }\n            if (end_tag) {\n''',
    '''            if (!ascii_byte(input_[probe])) {\n                const std::size_t scalar_bytes =\n                    detail::html_tokenizer_utf8_scalar_bytes_v1(input_, probe);\n                if (scalar_bytes == 0U) {\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer tag-open contains invalid UTF-8 scalar encoding");\n                }\n                // WHATWG tag-open/end-tag-open anything-else recovery is\n                // byte-preserving here: end tags become bogus comments; start\n                // tags emit '<' and reconsume the scalar in Data.\n            }\n            if (end_tag) {\n''',
    "Unicode tag-open recovery",
)

text = replace_once(
    text,
    '''            if (!ascii_byte(character)) {\n                // Preserve the historical census bucket until Unicode tag-name\n                // preprocessing/location authority is admitted.\n                return fail_data_tokenizer(\n                    error_,\n                    "HTML Data-tag tokenizer first attribute without separating whitespace is outside admitted v1 subset");\n            }\n            if (ascii_control_parse_error(character) &&\n''',
    '''            if (!ascii_byte(character)) {\n                const std::size_t scalar_bytes =\n                    detail::html_tokenizer_utf8_scalar_bytes_v1(input_, probe);\n                if (scalar_bytes == 0U) {\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer tag name contains invalid UTF-8 scalar encoding");\n                }\n                std::uint32_t scalar = 0U;\n                if (!decode_utf8_scalar_value(input_, probe, scalar_bytes, &scalar)) {\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer tag name scalar decoding failed");\n                }\n                if (unicode_control_parse_error(scalar) &&\n                    !emit_parse_error(probe, "control-character-in-input-stream")) {\n                    return false;\n                }\n                if (unicode_noncharacter(scalar) &&\n                    !emit_parse_error(probe, "noncharacter-in-input-stream")) {\n                    return false;\n                }\n                if (!append_bounded_bytes(\n                        &name,\n                        input_.substr(probe, scalar_bytes),\n                        "HTML Data-tag tokenizer tag name")) {\n                    return false;\n                }\n                probe += scalar_bytes;\n                continue;\n            }\n            if (ascii_control_parse_error(character) &&\n''',
    "Unicode tag-name authority",
)
path.write_text(text, encoding="utf-8")

path = ROOT / "tests/html_tokenizer_data_tag_recovery_v1_tests.cpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''    {\n        CollectingSink sink;\n        const std::string input("<\\xC3\\xA9>", 4U);\n        std::string error;\n        if (!require(\n                !tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),\n                "tag-open non-ASCII remains fail closed") ||\n            !require(\n                error.find("non-ASCII preprocessing/location authority") !=\n                    std::string::npos,\n                "tag-open non-ASCII failure remains explicit") ||\n            !require(sink.tokens.empty() && sink.errors.empty(),\n                     "tag-open non-ASCII publishes no recovery events")) {\n            return false;\n        }\n    }\n''',
    '''    {\n        CollectingSink sink;\n        HtmlTokenizerDataTagsV1Stats stats;\n        const std::string input("<\\xC3\\xA9>", 4U);\n        std::string error;\n        if (!require(\n                tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error),\n                std::string("tag-open Unicode recovery: ") + error) ||\n            !require(sink.tokens.size() == 1U &&\n                         sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&\n                         sink.tokens[0].data == input,\n                     "tag-open Unicode reconsumes scalar in Data") ||\n            !require(sink.errors.size() == 1U &&\n                         sink.errors[0].code ==\n                             "invalid-first-character-of-tag-name" &&\n                         sink.errors[0].line == 1U &&\n                         sink.errors[0].column == 2U,\n                     "tag-open Unicode exact recovery error") ||\n            !require(stats.tokens_emitted == 1U &&\n                         stats.character_tokens_emitted == 1U &&\n                         stats.parse_errors_emitted == 1U,\n                     "tag-open Unicode recovery stats")) {\n            return false;\n        }\n    }\n''',
    "tag-open Unicode focused regression",
)
path.write_text(text, encoding="utf-8")

print("applied Unicode tag-open/tag-name diagnostic")
