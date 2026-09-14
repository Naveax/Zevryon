#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
path = ROOT / "src/html_tokenizer_data_tags_v1.cpp"
text = path.read_text(encoding="utf-8")

anchor = '''bool ascii_control_parse_error(char value) noexcept {
    const auto byte = static_cast<unsigned char>(value);
    return (byte >= 0x01U && byte <= 0x08U) || byte == 0x0BU ||
        (byte >= 0x0EU && byte <= 0x1FU) || byte == 0x7FU;
}
'''
insert = anchor + r'''

bool decode_utf8_scalar_value(
    std::string_view input,
    std::size_t offset,
    std::size_t scalar_bytes,
    std::uint32_t* value) noexcept {
    if (value == nullptr || scalar_bytes < 2U || scalar_bytes > 4U ||
        offset > input.size() || scalar_bytes > input.size() - offset) {
        return false;
    }
    const auto first = static_cast<unsigned char>(input[offset]);
    std::uint32_t scalar = scalar_bytes == 2U
        ? static_cast<std::uint32_t>(first & 0x1FU)
        : scalar_bytes == 3U
            ? static_cast<std::uint32_t>(first & 0x0FU)
            : static_cast<std::uint32_t>(first & 0x07U);
    for (std::size_t index = 1U; index < scalar_bytes; ++index) {
        const auto continuation = static_cast<unsigned char>(input[offset + index]);
        scalar = (scalar << 6U) | static_cast<std::uint32_t>(continuation & 0x3FU);
    }
    *value = scalar;
    return true;
}

bool unicode_noncharacter(std::uint32_t scalar) noexcept {
    return (scalar >= 0xFDD0U && scalar <= 0xFDEFU) ||
        (scalar <= 0x10FFFFU && (scalar & 0xFFFFU) >= 0xFFFEU);
}
'''
if text.count(anchor) != 1:
    raise SystemExit(f"unicode helper anchor count={text.count(anchor)}")
text = text.replace(anchor, insert, 1)

old = '''                if (!append_bounded_bytes(
                        &character_buffer_,
                        input_.substr(cursor, scalar_bytes),
                        "HTML Data-tag tokenizer coalesced character token")) {
                    return false;
                }
                cursor += scalar_bytes;
                continue;
            }
            if (character == '&') {
'''
new = '''                std::uint32_t scalar = 0U;
                if (!decode_utf8_scalar_value(input_, cursor, scalar_bytes, &scalar)) {
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer raw Data text scalar decoding failed");
                }
                if (unicode_noncharacter(scalar) &&
                    !emit_parse_error(cursor, "noncharacter-in-input-stream")) {
                    return false;
                }
                if (!append_bounded_bytes(
                        &character_buffer_,
                        input_.substr(cursor, scalar_bytes),
                        "HTML Data-tag tokenizer coalesced character token")) {
                    return false;
                }
                cursor += scalar_bytes;
                continue;
            }
            if (ascii_control_parse_error(character) &&
                !emit_parse_error(cursor, "control-character-in-input-stream")) {
                return false;
            }
            if (character == '&') {
'''
if text.count(old) != 1:
    raise SystemExit(f"Data Unicode run anchor count={text.count(old)}")
text = text.replace(old, new, 1)
path.write_text(text, encoding="utf-8")
print("applied Data invalid-Unicode parse-error diagnostic patch")
