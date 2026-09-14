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
quoted_old = '''                if (!ascii_byte(character)) {\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer non-ASCII attribute-value authority is not implemented");\n                }\n                if (character == '&') {\n'''
quoted_new = '''                if (!ascii_byte(character)) {\n                    const std::size_t scalar_bytes =\n                        detail::html_tokenizer_utf8_scalar_bytes_v1(input_, *cursor);\n                    if (scalar_bytes == 0U) {\n                        return fail_data_tokenizer(\n                            error_,\n                            "HTML Data-tag tokenizer attribute value contains invalid UTF-8 scalar encoding");\n                    }\n                    std::uint32_t scalar = 0U;\n                    if (!decode_utf8_scalar_value(\n                            input_, *cursor, scalar_bytes, &scalar)) {\n                        return fail_data_tokenizer(\n                            error_,\n                            "HTML Data-tag tokenizer attribute value scalar decoding failed");\n                    }\n                    if (unicode_noncharacter(scalar) &&\n                        !emit_parse_error(*cursor, "noncharacter-in-input-stream")) {\n                        return false;\n                    }\n                    if (!append_bounded_bytes(\n                            value,\n                            input_.substr(*cursor, scalar_bytes),\n                            "HTML Data-tag tokenizer attribute value")) {\n                        return false;\n                    }\n                    *cursor += scalar_bytes;\n                    continue;\n                }\n                if (character == '&') {\n'''
text = replace_once(text, quoted_old, quoted_new, "quoted Unicode attribute value")
unquoted_old = '''            if (!ascii_byte(character)) {\n                return fail_data_tokenizer(\n                    error_,\n                    "HTML Data-tag tokenizer non-ASCII attribute-value authority is not implemented");\n            }\n            if (character == '&') {\n'''
unquoted_new = '''            if (!ascii_byte(character)) {\n                const std::size_t scalar_bytes =\n                    detail::html_tokenizer_utf8_scalar_bytes_v1(input_, *cursor);\n                if (scalar_bytes == 0U) {\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer attribute value contains invalid UTF-8 scalar encoding");\n                }\n                std::uint32_t scalar = 0U;\n                if (!decode_utf8_scalar_value(\n                        input_, *cursor, scalar_bytes, &scalar)) {\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer attribute value scalar decoding failed");\n                }\n                if (unicode_noncharacter(scalar) &&\n                    !emit_parse_error(*cursor, "noncharacter-in-input-stream")) {\n                    return false;\n                }\n                if (!append_bounded_bytes(\n                        value,\n                        input_.substr(*cursor, scalar_bytes),\n                        "HTML Data-tag tokenizer attribute value")) {\n                    return false;\n                }\n                *cursor += scalar_bytes;\n                continue;\n            }\n            if (character == '&') {\n'''
text = replace_once(text, unquoted_old, unquoted_new, "unquoted Unicode attribute value")
path.write_text(text, encoding="utf-8")
print("applied Unicode attribute-value diagnostic")
