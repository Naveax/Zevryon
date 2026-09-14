#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)

path = ROOT / "src/html_tokenizer_script_data_v1.cpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '#include "html_tokenizer_script_data_v1.hpp"\n',
    '#include "html_tokenizer_script_data_v1.hpp"\n\n#include "html_tokenizer_utf8_v1.hpp"\n',
    "UTF-8 helper include",
)
text = replace_once(
    text,
    '''bool ascii_space(char value) noexcept {
    return value == ' ' || value == '\\t' || value == '\\n' ||
        value == '\\r' || value == '\\f';
}

char ascii_lower(char value) noexcept {
''',
    '''bool ascii_space(char value) noexcept {
    return value == ' ' || value == '\\t' || value == '\\n' ||
        value == '\\r' || value == '\\f';
}

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
        scalar = (scalar << 6U) |
            static_cast<std::uint32_t>(continuation & 0x3FU);
    }
    *value = scalar;
    return true;
}

bool unicode_noncharacter(std::uint32_t scalar) noexcept {
    return (scalar >= 0xFDD0U && scalar <= 0xFDEFU) ||
        (scalar <= 0x10FFFFU && (scalar & 0xFFFFU) >= 0xFFFEU);
}

char ascii_lower(char value) noexcept {
''',
    "Unicode scalar helpers",
)
text = replace_once(
    text,
    '''        while (cursor < input_.size() && !done_) {
            if (!ascii_byte(input_[cursor])) {
                stats_->bytes_consumed = static_cast<std::uint64_t>(cursor);
                return fail_script(
                    error_,
                    "HTML Script-data non-ASCII preprocessing/location authority is not implemented");
            }
            if (!consume_state(&cursor)) {
''',
    '''        while (cursor < input_.size() && !done_) {
            if (!ascii_byte(input_[cursor])) {
                const std::size_t scalar_bytes =
                    detail::html_tokenizer_utf8_scalar_bytes_v1(input_, cursor);
                if (scalar_bytes == 0U) {
                    stats_->bytes_consumed = static_cast<std::uint64_t>(cursor);
                    return fail_script(
                        error_,
                        "HTML Script-data contains invalid UTF-8 scalar encoding");
                }
                std::uint32_t scalar = 0U;
                if (!decode_utf8_scalar_value(
                        input_, cursor, scalar_bytes, &scalar)) {
                    stats_->bytes_consumed = static_cast<std::uint64_t>(cursor);
                    return fail_script(
                        error_,
                        "HTML Script-data scalar decoding failed");
                }
                if (unicode_noncharacter(scalar) &&
                    !emit_parse_error(cursor, "noncharacter-in-input-stream")) {
                    stats_->bytes_consumed = static_cast<std::uint64_t>(cursor);
                    return false;
                }
                if (!append_characters(input_.substr(cursor, scalar_bytes))) {
                    stats_->bytes_consumed = static_cast<std::uint64_t>(cursor);
                    return false;
                }
                cursor += scalar_bytes;
                stats_->bytes_consumed = static_cast<std::uint64_t>(cursor);
                continue;
            }
            if (!consume_state(&cursor)) {
''',
    "Script Unicode scalar admission",
)
path.write_text(text, encoding="utf-8")

path = ROOT / "tests/html_tokenizer_script_data_v1_tests.cpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''    {
        const std::string input("a\\xC3\\xA9", 3U);
        CollectingSink sink;
        HtmlTokenizerScriptDataV1Stats stats;
        HtmlTokenizerScriptDataV1Result result;
        std::string error;
        if (!require(
                !consume_html_script_data_v1(
                    input,
                    "script",
                    {},
                    &sink,
                    &stats,
                    &result,
                    &error),
                "non-ASCII remains fail closed") ||
            !require(error.find("non-ASCII preprocessing/location authority") !=
                         std::string::npos,
                     "non-ASCII failure identifies location/preprocessing debt") ||
            !require(sink.tokens.empty() && sink.errors.empty(),
                     "non-ASCII failure does not flush partial character data") ||
            !require(stats.bytes_consumed == 1U,
                     "non-ASCII failure reports consumed ASCII prefix")) {
            return false;
        }
    }
''',
    '''    {
        const std::string input("a\\xC3\\xA9", 3U);
        CollectingSink sink;
        HtmlTokenizerScriptDataV1Stats stats;
        HtmlTokenizerScriptDataV1Result result;
        std::string error;
        if (!require(
                consume_html_script_data_v1(
                    input,
                    "script",
                    {},
                    &sink,
                    &stats,
                    &result,
                    &error),
                std::string("Unicode Script-data scalar: ") + error) ||
            !require(sink.tokens.size() == 1U &&
                         sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                         sink.tokens[0].data == input,
                     "Unicode Script-data scalar payload") ||
            !require(sink.errors.empty(),
                     "Unicode Script-data scalar has no parse error") ||
            !require(stats.bytes_consumed == input.size(),
                     "Unicode Script-data scalar byte accounting")) {
            return false;
        }
    }
''',
    "Script Unicode focused regression",
)
path.write_text(text, encoding="utf-8")

print("applied Script-data Unicode production candidate after zero-failure fixes")
