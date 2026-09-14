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
    '''                if (!ascii_byte(character)) {\n                    // Keep the historical v3 census classification stable until\n                    // Unicode preprocessing/location authority is admitted.\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer attribute name byte is outside admitted v1 subset");\n                }\n''',
    '''                if (!ascii_byte(character)) {\n                    const std::size_t scalar_bytes =\n                        detail::html_tokenizer_utf8_scalar_bytes_v1(input_, *cursor);\n                    if (scalar_bytes == 0U) {\n                        return fail_data_tokenizer(\n                            error_,\n                            "HTML Data-tag tokenizer attribute name contains invalid UTF-8 scalar encoding");\n                    }\n                    std::uint32_t scalar = 0U;\n                    if (!decode_utf8_scalar_value(\n                            input_, *cursor, scalar_bytes, &scalar)) {\n                        return fail_data_tokenizer(\n                            error_,\n                            "HTML Data-tag tokenizer attribute name scalar decoding failed");\n                    }\n                    if (unicode_control_parse_error(scalar) &&\n                        !emit_parse_error(*cursor, "control-character-in-input-stream")) {\n                        return false;\n                    }\n                    if (unicode_noncharacter(scalar) &&\n                        !emit_parse_error(*cursor, "noncharacter-in-input-stream")) {\n                        return false;\n                    }\n                    if (!append_bounded_bytes(\n                            &attribute.name,\n                            input_.substr(*cursor, scalar_bytes),\n                            "HTML Data-tag tokenizer attribute name")) {\n                        return false;\n                    }\n                    *cursor += scalar_bytes;\n                    continue;\n                }\n''',
    "Unicode attribute-name authority",
)

path.write_text(text, encoding="utf-8")
print("applied Unicode attribute-name diagnostic")
