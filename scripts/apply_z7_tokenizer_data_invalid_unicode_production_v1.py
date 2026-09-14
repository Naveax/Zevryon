#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


# Production Data-state input-stream diagnostics for C0/DEL and Unicode noncharacters.
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
text = replace_once(text, anchor, insert, "Data Unicode helper authority")
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
text = replace_once(text, old, new, "Data invalid-Unicode parse-error semantics")
path.write_text(text, encoding="utf-8")


# Focused direct regression authority.
path = ROOT / "tests/html_tokenizer_data_utf8_text_v1_tests.cpp"
text = path.read_text(encoding="utf-8")
anchor = '''bool test_invalid_utf8_fails_closed() {
'''
insert = r'''bool test_data_control_character_error() {
    const std::string input("A\x0B" "B", 3U);
    CollectingSink sink;
    HtmlTokenizerDataTagsV1Stats stats;
    std::string error;
    return require(tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error), error) &&
        require(sink.tokens.size() == 1U &&
                    sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                    sink.tokens[0].data == input,
                "Data C0 control payload is preserved") &&
        require(sink.errors.size() == 1U &&
                    sink.errors[0].code == "control-character-in-input-stream" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == 2U,
                "Data C0 control exact parse error") &&
        require(stats.parse_errors_emitted == 1U,
                "Data C0 control parse-error accounting");
}

bool test_data_noncharacter_error() {
    const std::string noncharacter("\xEF\xB7\x90", 3U); // U+FDD0
    const std::string input = "A" + noncharacter + "B";
    CollectingSink sink;
    HtmlTokenizerDataTagsV1Stats stats;
    std::string error;
    return require(tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error), error) &&
        require(sink.tokens.size() == 1U &&
                    sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                    sink.tokens[0].data == input,
                "Data noncharacter payload is preserved") &&
        require(sink.errors.size() == 1U &&
                    sink.errors[0].code == "noncharacter-in-input-stream" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == 2U,
                "Data noncharacter exact parse error") &&
        require(stats.parse_errors_emitted == 1U,
                "Data noncharacter parse-error accounting");
}

bool test_plane_end_noncharacter_error() {
    const std::string input("\xF4\x8F\xBF\xBF", 4U); // U+10FFFF
    CollectingSink sink;
    HtmlTokenizerDataTagsV1Stats stats;
    std::string error;
    return require(tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error), error) &&
        require(sink.tokens.size() == 1U && sink.tokens[0].data == input,
                "plane-end noncharacter payload is preserved") &&
        require(sink.errors.size() == 1U &&
                    sink.errors[0].code == "noncharacter-in-input-stream" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == 1U,
                "plane-end noncharacter exact parse error");
}

'''
text = replace_once(text, anchor, insert + anchor, "Data invalid-Unicode focused tests")
old = '''            test_scalar_aware_error_columns() &&
            test_invalid_utf8_fails_closed() &&
'''
new = '''            test_scalar_aware_error_columns() &&
            test_data_control_character_error() &&
            test_data_noncharacter_error() &&
            test_plane_end_noncharacter_error() &&
            test_invalid_utf8_fails_closed() &&
'''
text = replace_once(text, old, new, "Data invalid-Unicode test registration")
path.write_text(text, encoding="utf-8")


# Tighten the documented admitted boundary without broadening tag/attribute authority.
path = ROOT / "docs/Z7_HTML_TOKENIZER_DATA_TAGS_V1.md"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''`tokenize_html_data_tags_v1()` admits ASCII raw Data-state inputs containing:
''',
    '''`tokenize_html_data_tags_v1()` admits bounded raw Data-state inputs containing:
''',
    "Data docs admitted-input heading",
)
text = replace_once(
    text,
    '''- coalesced Character tokens outside tags, including ordinary Data-state U+0000 with `unexpected-null-character` while preserving the raw U+0000 character token payload;
''',
    '''- coalesced Character tokens outside tags, including ordinary Data-state U+0000 with `unexpected-null-character` while preserving the raw U+0000 character token payload;
- well-formed UTF-8 scalar text in ordinary Data, preserving the original UTF-8 bytes;
- Data-state C0/DEL input controls with `control-character-in-input-stream`, preserving the source byte;
- Unicode noncharacters in ordinary Data with `noncharacter-in-input-stream`, including U+FDD0..U+FDEF and every plane-ending U+FFFE/U+FFFF scalar;
''',
    "Data docs input-stream diagnostics",
)
text = replace_once(
    text,
    '''Parse-error line/column positions are one-based. V1 raw-input location authority remains deliberately ASCII-only, so source byte offsets and source character columns are identical. Non-ASCII raw input remains fail-closed until preprocessing and Unicode location accounting are admitted.
''',
    '''Parse-error line/column positions are one-based. Ordinary Data text uses UTF-8 scalar-aware source columns, including two UTF-16 code units for non-BMP scalars where html5lib location authority requires it. Tag-name and attribute raw-input authority remains deliberately narrower and continues to fail closed on unadmitted non-ASCII transitions.
''',
    "Data docs location authority",
)
text = replace_once(
    text,
    '''- non-ASCII raw-input preprocessing/location authority, including non-ASCII tag and attribute names;
''',
    '''- non-ASCII tag-name, attribute-name and attribute-value transition authority outside the explicitly admitted ordinary Data-text path;
''',
    "Data docs fail-closed Unicode boundary",
)
text = replace_once(
    text,
    '''Raw-input preprocessing/non-ASCII authority, broader recovery, CDATA and tree-builder conformance remain open.
''',
    '''Remaining tag/attribute preprocessing authority, broader recovery, CDATA and tree-builder conformance remain open.
''',
    "Data docs claim boundary",
)
path.write_text(text, encoding="utf-8")

print("applied Data invalid-Unicode production slice")
