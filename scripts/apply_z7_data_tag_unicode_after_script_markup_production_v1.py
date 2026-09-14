#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


def replace_count(text: str, old: str, new: str, expected: int, label: str) -> str:
    count = text.count(old)
    if count != expected:
        raise SystemExit(f"{label}: expected {expected} anchors, found {count}")
    return text.replace(old, new)


path = ROOT / "src/html_tokenizer_data_tags_v1.cpp"
text = path.read_text(encoding="utf-8")

text = replace_once(
    text,
    '''bool unicode_noncharacter(std::uint32_t scalar) noexcept {
    return (scalar >= 0xFDD0U && scalar <= 0xFDEFU) ||
        (scalar <= 0x10FFFFU && (scalar & 0xFFFFU) >= 0xFFFEU);
}

char ascii_lower(char value) noexcept {
''',
    '''bool unicode_noncharacter(std::uint32_t scalar) noexcept {
    return (scalar >= 0xFDD0U && scalar <= 0xFDEFU) ||
        (scalar <= 0x10FFFFU && (scalar & 0xFFFFU) >= 0xFFFEU);
}

bool unicode_control_parse_error(std::uint32_t scalar) noexcept {
    return scalar >= 0x80U && scalar <= 0x9FU;
}

char ascii_lower(char value) noexcept {
''',
    "Unicode control helper",
)

text = replace_once(
    text,
    '''    bool append_bounded_bytes(
        std::string* value,
        std::string_view bytes,
        std::string_view label) {
        if (value->size() > config_.maximum_token_bytes ||
            bytes.size() > config_.maximum_token_bytes - value->size()) {
            return fail_data_tokenizer(
                error_,
                std::string(label) + " exceeds bounded byte limit");
        }
        value->append(bytes.data(), bytes.size());
        return true;
    }

    bool consume_character_reference(
''',
    '''    bool append_bounded_bytes(
        std::string* value,
        std::string_view bytes,
        std::string_view label) {
        if (value->size() > config_.maximum_token_bytes ||
            bytes.size() > config_.maximum_token_bytes - value->size()) {
            return fail_data_tokenizer(
                error_,
                std::string(label) + " exceeds bounded byte limit");
        }
        value->append(bytes.data(), bytes.size());
        return true;
    }

    bool append_unicode_scalar(
        std::size_t* cursor,
        std::string* destination,
        std::string_view label) {
        if (cursor == nullptr || destination == nullptr || *cursor >= input_.size() ||
            ascii_byte(input_[*cursor])) {
            return fail_data_tokenizer(
                error_,
                "HTML Data-tag tokenizer Unicode scalar dispatch invariant failed");
        }
        const std::size_t scalar_bytes =
            detail::html_tokenizer_utf8_scalar_bytes_v1(input_, *cursor);
        if (scalar_bytes == 0U) {
            return fail_data_tokenizer(
                error_,
                std::string(label) + " contains invalid UTF-8 scalar encoding");
        }
        std::uint32_t scalar = 0U;
        if (!decode_utf8_scalar_value(input_, *cursor, scalar_bytes, &scalar)) {
            return fail_data_tokenizer(
                error_,
                std::string(label) + " scalar decoding failed");
        }
        if (unicode_control_parse_error(scalar) &&
            !emit_parse_error(*cursor, "control-character-in-input-stream")) {
            return false;
        }
        if (unicode_noncharacter(scalar) &&
            !emit_parse_error(*cursor, "noncharacter-in-input-stream")) {
            return false;
        }
        if (!append_bounded_bytes(
                destination,
                input_.substr(*cursor, scalar_bytes),
                label)) {
            return false;
        }
        *cursor += scalar_bytes;
        return true;
    }

    bool consume_character_reference(
''',
    "Unicode scalar append helper",
)

text = replace_once(
    text,
    '''            if (!ascii_byte(character)) {
                const std::size_t scalar_bytes =
                    detail::html_tokenizer_utf8_scalar_bytes_v1(input_, scan);
                if (scalar_bytes == 0U) {
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer bogus comment contains invalid UTF-8 scalar encoding");
                }
                if (!append_bounded_bytes(
                        &data,
                        input_.substr(scan, scalar_bytes),
                        "HTML Data-tag tokenizer bogus comment token")) {
                    return false;
                }
                scan += scalar_bytes;
                continue;
            }
''',
    '''            if (!ascii_byte(character)) {
                if (!append_unicode_scalar(
                        &scan,
                        &data,
                        "HTML Data-tag tokenizer bogus comment token")) {
                    return false;
                }
                continue;
            }
''',
    "bogus-comment Unicode scalar",
)

text = replace_count(
    text,
    '''                if (!ascii_byte(character)) {
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer non-ASCII attribute-value authority is not implemented");
                }
''',
    '''                if (!ascii_byte(character)) {
                    if (!append_unicode_scalar(
                            cursor,
                            value,
                            "HTML Data-tag tokenizer attribute value")) {
                        return false;
                    }
                    continue;
                }
''',
    2,
    "attribute-value Unicode scalar",
)

text = replace_once(
    text,
    '''                if (!ascii_byte(character)) {
                    // Keep the historical v3 census classification stable until
                    // Unicode preprocessing/location authority is admitted.
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer attribute name byte is outside admitted v1 subset");
                }
''',
    '''                if (!ascii_byte(character)) {
                    if (!append_unicode_scalar(
                            cursor,
                            &attribute.name,
                            "HTML Data-tag tokenizer attribute name")) {
                        return false;
                    }
                    continue;
                }
''',
    "attribute-name Unicode scalar",
)

text = replace_once(
    text,
    '''        if (!ascii_alpha(input_[probe])) {
            if (!ascii_byte(input_[probe])) {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer non-ASCII preprocessing/location authority is not implemented");
            }
            if (end_tag) {
''',
    '''        if (!ascii_alpha(input_[probe])) {
            if (!ascii_byte(input_[probe])) {
                const std::size_t scalar_bytes =
                    detail::html_tokenizer_utf8_scalar_bytes_v1(input_, probe);
                if (scalar_bytes == 0U) {
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer tag-open contains invalid UTF-8 scalar encoding");
                }
                std::uint32_t scalar = 0U;
                if (!decode_utf8_scalar_value(input_, probe, scalar_bytes, &scalar)) {
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer tag-open scalar decoding failed");
                }
                if (unicode_control_parse_error(scalar) || unicode_noncharacter(scalar)) {
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer tag-open Unicode input-error ordering authority is not implemented");
                }
            }
            if (end_tag) {
''',
    "tag-open Unicode scalar",
)

text = replace_once(
    text,
    '''            if (!ascii_byte(character)) {
                // Preserve the historical census bucket until Unicode tag-name
                // preprocessing/location authority is admitted.
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer first attribute without separating whitespace is outside admitted v1 subset");
            }
''',
    '''            if (!ascii_byte(character)) {
                if (!append_unicode_scalar(
                        &probe,
                        &name,
                        "HTML Data-tag tokenizer tag name")) {
                    return false;
                }
                continue;
            }
''',
    "tag-name Unicode scalar",
)

text = replace_once(
    text,
    '''                const std::size_t reconsume_offset = probe + 1U;
                const char reconsume_character = input_[reconsume_offset];
                if (!ascii_byte(reconsume_character)) {
                    // Do not move the existing non-ASCII debt into a new census
                    // bucket before Unicode tag-name authority is admitted.
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer first attribute without separating whitespace is outside admitted v1 subset");
                }
                leading_control_error_emitted =
                    ascii_control_parse_error(reconsume_character);
''',
    '''                const std::size_t reconsume_offset = probe + 1U;
                const char reconsume_character = input_[reconsume_offset];
                if (!ascii_byte(reconsume_character)) {
                    const std::size_t scalar_bytes =
                        detail::html_tokenizer_utf8_scalar_bytes_v1(input_, reconsume_offset);
                    if (scalar_bytes == 0U) {
                        return fail_data_tokenizer(
                            error_,
                            "HTML Data-tag tokenizer self-closing reconsume contains invalid UTF-8 scalar encoding");
                    }
                    std::uint32_t scalar = 0U;
                    if (!decode_utf8_scalar_value(
                            input_, reconsume_offset, scalar_bytes, &scalar)) {
                        return fail_data_tokenizer(
                            error_,
                            "HTML Data-tag tokenizer self-closing reconsume scalar decoding failed");
                    }
                    if (unicode_control_parse_error(scalar) || unicode_noncharacter(scalar)) {
                        return fail_data_tokenizer(
                            error_,
                            "HTML Data-tag tokenizer self-closing Unicode input-error ordering authority is not implemented");
                    }
                }
                leading_control_error_emitted =
                    ascii_byte(reconsume_character) &&
                    ascii_control_parse_error(reconsume_character);
''',
    "self-closing Unicode reconsume",
)

path.write_text(text, encoding="utf-8")

path = ROOT / "tests/html_tokenizer_data_tags_v1_tests.cpp"
text = path.read_text(encoding="utf-8")

unicode_test = r'''bool test_unicode_tag_and_attribute_states() {
    const std::string scalar("\xF4\x80\x80\x80", 4U); // U+100000
    {
        CollectingSink sink;
        std::string error;
        const std::string input = "<a" + scalar + ">";
        if (!require(
                tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                std::string("Unicode tag name: ") + error) ||
            !require(sink.tokens.size() == 1U, "Unicode tag-name token count") ||
            !require(
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::StartTag &&
                    sink.tokens[0].name == "a" + scalar,
                "Unicode scalar is retained in tag name") ||
            !require(sink.errors.empty(), "Unicode tag name has no parse errors")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        const std::string input = "<" + scalar;
        if (!require(
                tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                std::string("Unicode tag-open reconsume: ") + error) ||
            !require(sink.tokens.size() == 1U, "Unicode tag-open token count") ||
            !require(
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                    sink.tokens[0].data == "<" + scalar,
                "Unicode tag-open falls back to Data") ||
            !require(
                sink.errors.size() == 1U &&
                    sink.errors[0].code == "invalid-first-character-of-tag-name" &&
                    sink.errors[0].column == 2U,
                "Unicode tag-open exact diagnostic")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        const std::string input = "</" + scalar;
        if (!require(
                tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                std::string("Unicode end-tag bogus comment: ") + error) ||
            !require(sink.tokens.size() == 1U, "Unicode end-tag bogus-comment token count") ||
            !require(
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Comment &&
                    sink.tokens[0].data == scalar,
                "Unicode end-tag bogus-comment payload") ||
            !require(
                sink.errors.size() == 1U &&
                    sink.errors[0].code == "invalid-first-character-of-tag-name" &&
                    sink.errors[0].column == 3U,
                "Unicode end-tag exact diagnostic")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        const std::string input = "<a " + scalar + "='" + scalar + "' b=" + scalar + ">";
        if (!require(
                tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                std::string("Unicode attributes: ") + error) ||
            !require(sink.tokens.size() == 1U, "Unicode attribute token count") ||
            !require(sink.tokens[0].attributes.size() == 2U, "Unicode attribute count") ||
            !require(attribute_is(sink.tokens[0].attributes[0], scalar, scalar),
                     "Unicode attribute name and quoted value") ||
            !require(attribute_is(sink.tokens[0].attributes[1], "b", scalar),
                     "Unicode unquoted attribute value") ||
            !require(sink.errors.empty(), "Unicode attributes have no parse errors")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        const std::string input = "<a a=''" + scalar + ">";
        if (!require(
                tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                std::string("Unicode missing-whitespace attribute: ") + error) ||
            !require(sink.tokens.size() == 1U, "Unicode missing-whitespace token count") ||
            !require(sink.tokens[0].attributes.size() == 2U,
                     "Unicode missing-whitespace attributes survive") ||
            !require(attribute_is(sink.tokens[0].attributes[0], "a", ""),
                     "Unicode missing-whitespace first attribute") ||
            !require(attribute_is(sink.tokens[0].attributes[1], scalar, ""),
                     "Unicode missing-whitespace second attribute") ||
            !require(
                sink.errors.size() == 1U &&
                    sink.errors[0].code == "missing-whitespace-between-attributes" &&
                    sink.errors[0].column == 8U,
                "Unicode missing-whitespace exact diagnostic")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        const std::string input = "<a/" + scalar + ">";
        if (!require(
                tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                std::string("Unicode self-closing reconsume: ") + error) ||
            !require(sink.tokens.size() == 1U, "Unicode solidus token count") ||
            !require(sink.tokens[0].attributes.size() == 1U,
                     "Unicode solidus reconsume attribute count") ||
            !require(attribute_is(sink.tokens[0].attributes[0], scalar, ""),
                     "Unicode solidus reconsume attribute") ||
            !require(
                sink.errors.size() == 1U &&
                    sink.errors[0].code == "unexpected-solidus-in-tag" &&
                    sink.errors[0].column == 4U,
                "Unicode solidus reconsume exact diagnostic")) {
            return false;
        }
    }
    return true;
}

'''

text = replace_once(
    text,
    '''bool test_token_and_attribute_bounds() {
''',
    unicode_test + '''bool test_token_and_attribute_bounds() {
''',
    "focused Unicode state regression",
)
text = replace_once(
    text,
    '''        !test_admitted_references_and_fail_closed_boundaries() ||
        !test_token_and_attribute_bounds()) {
''',
    '''        !test_admitted_references_and_fail_closed_boundaries() ||
        !test_unicode_tag_and_attribute_states() ||
        !test_token_and_attribute_bounds()) {
''',
    "focused Unicode state test registration",
)
path.write_text(text, encoding="utf-8")

print("applied bounded Data-tag Unicode production candidate")
