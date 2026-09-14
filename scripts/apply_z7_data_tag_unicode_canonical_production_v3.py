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
    '''                if (!ascii_byte(character)) {\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer non-ASCII attribute-value authority is not implemented");\n                }\n                if (ascii_control_parse_error(character) &&\n''',
    '''                if (!ascii_byte(character)) {\n                    const std::size_t scalar_bytes =\n                        detail::html_tokenizer_utf8_scalar_bytes_v1(input_, *cursor);\n                    if (scalar_bytes == 0U) {\n                        return fail_data_tokenizer(\n                            error_,\n                            "HTML Data-tag tokenizer attribute value contains invalid UTF-8 scalar encoding");\n                    }\n                    std::uint32_t scalar = 0U;\n                    if (!decode_utf8_scalar_value(\n                            input_, *cursor, scalar_bytes, &scalar)) {\n                        return fail_data_tokenizer(\n                            error_,\n                            "HTML Data-tag tokenizer attribute value scalar decoding failed");\n                    }\n                    if (unicode_control_parse_error(scalar) &&\n                        !emit_parse_error(*cursor, "control-character-in-input-stream")) {\n                        return false;\n                    }\n                    if (unicode_noncharacter(scalar) &&\n                        !emit_parse_error(*cursor, "noncharacter-in-input-stream")) {\n                        return false;\n                    }\n                    if (!append_bounded_bytes(\n                            value,\n                            input_.substr(*cursor, scalar_bytes),\n                            "HTML Data-tag tokenizer attribute value")) {\n                        return false;\n                    }\n                    *cursor += scalar_bytes;\n                    continue;\n                }\n                if (ascii_control_parse_error(character) &&\n''',
    "quoted Unicode attribute value",
)
text = replace_once(
    text,
    '''            if (!ascii_byte(character)) {\n                return fail_data_tokenizer(\n                    error_,\n                    "HTML Data-tag tokenizer non-ASCII attribute-value authority is not implemented");\n            }\n            if (ascii_control_parse_error(character) &&\n''',
    '''            if (!ascii_byte(character)) {\n                const std::size_t scalar_bytes =\n                    detail::html_tokenizer_utf8_scalar_bytes_v1(input_, *cursor);\n                if (scalar_bytes == 0U) {\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer attribute value contains invalid UTF-8 scalar encoding");\n                }\n                std::uint32_t scalar = 0U;\n                if (!decode_utf8_scalar_value(\n                        input_, *cursor, scalar_bytes, &scalar)) {\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer attribute value scalar decoding failed");\n                }\n                if (unicode_control_parse_error(scalar) &&\n                    !emit_parse_error(*cursor, "control-character-in-input-stream")) {\n                    return false;\n                }\n                if (unicode_noncharacter(scalar) &&\n                    !emit_parse_error(*cursor, "noncharacter-in-input-stream")) {\n                    return false;\n                }\n                if (!append_bounded_bytes(\n                        value,\n                        input_.substr(*cursor, scalar_bytes),\n                        "HTML Data-tag tokenizer attribute value")) {\n                    return false;\n                }\n                *cursor += scalar_bytes;\n                continue;\n            }\n            if (ascii_control_parse_error(character) &&\n''',
    "unquoted Unicode attribute value",
)
text = replace_once(
    text,
    '''                if (!ascii_byte(character)) {\n                    // Keep the historical v3 census classification stable until\n                    // Unicode preprocessing/location authority is admitted.\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer attribute name byte is outside admitted v1 subset");\n                }\n''',
    '''                if (!ascii_byte(character)) {\n                    const std::size_t scalar_bytes =\n                        detail::html_tokenizer_utf8_scalar_bytes_v1(input_, *cursor);\n                    if (scalar_bytes == 0U) {\n                        return fail_data_tokenizer(\n                            error_,\n                            "HTML Data-tag tokenizer attribute name contains invalid UTF-8 scalar encoding");\n                    }\n                    std::uint32_t scalar = 0U;\n                    if (!decode_utf8_scalar_value(\n                            input_, *cursor, scalar_bytes, &scalar)) {\n                        return fail_data_tokenizer(\n                            error_,\n                            "HTML Data-tag tokenizer attribute name scalar decoding failed");\n                    }\n                    if (unicode_control_parse_error(scalar) &&\n                        !(leading_control_error_emitted && *cursor == name_begin) &&\n                        !emit_parse_error(*cursor, "control-character-in-input-stream")) {\n                        return false;\n                    }\n                    if (unicode_noncharacter(scalar) &&\n                        !emit_parse_error(*cursor, "noncharacter-in-input-stream")) {\n                        return false;\n                    }\n                    if (!append_bounded_bytes(\n                            &attribute.name,\n                            input_.substr(*cursor, scalar_bytes),\n                            "HTML Data-tag tokenizer attribute name")) {\n                        return false;\n                    }\n                    *cursor += scalar_bytes;\n                    continue;\n                }\n''',
    "Unicode attribute name",
)
text = replace_once(
    text,
    '''            if (!ascii_byte(input_[probe])) {\n                return fail_data_tokenizer(\n                    error_,\n                    "HTML Data-tag tokenizer non-ASCII preprocessing/location authority is not implemented");\n            }\n            if (end_tag) {\n''',
    '''            if (!ascii_byte(input_[probe])) {\n                const std::size_t scalar_bytes =\n                    detail::html_tokenizer_utf8_scalar_bytes_v1(input_, probe);\n                if (scalar_bytes == 0U) {\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer tag-open contains invalid UTF-8 scalar encoding");\n                }\n                std::uint32_t scalar = 0U;\n                if (!decode_utf8_scalar_value(input_, probe, scalar_bytes, &scalar)) {\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer tag-open scalar decoding failed");\n                }\n                if (unicode_control_parse_error(scalar) || unicode_noncharacter(scalar)) {\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer tag-open Unicode input-error ordering is outside admitted v1 subset");\n                }\n            }\n            if (end_tag) {\n''',
    "Unicode tag-open recovery",
)
text = replace_once(
    text,
    '''            if (!ascii_byte(character)) {\n                // Preserve the historical census bucket until Unicode tag-name\n                // preprocessing/location authority is admitted.\n                return fail_data_tokenizer(\n                    error_,\n                    "HTML Data-tag tokenizer first attribute without separating whitespace is outside admitted v1 subset");\n            }\n''',
    '''            if (!ascii_byte(character)) {\n                const std::size_t scalar_bytes =\n                    detail::html_tokenizer_utf8_scalar_bytes_v1(input_, probe);\n                if (scalar_bytes == 0U) {\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer tag name contains invalid UTF-8 scalar encoding");\n                }\n                std::uint32_t scalar = 0U;\n                if (!decode_utf8_scalar_value(input_, probe, scalar_bytes, &scalar)) {\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer tag name scalar decoding failed");\n                }\n                if (unicode_control_parse_error(scalar) &&\n                    !emit_parse_error(probe, "control-character-in-input-stream")) {\n                    return false;\n                }\n                if (unicode_noncharacter(scalar) &&\n                    !emit_parse_error(probe, "noncharacter-in-input-stream")) {\n                    return false;\n                }\n                if (!append_bounded_bytes(\n                        &name,\n                        input_.substr(probe, scalar_bytes),\n                        "HTML Data-tag tokenizer tag name")) {\n                    return false;\n                }\n                probe += scalar_bytes;\n                continue;\n            }\n''',
    "Unicode tag name",
)
text = replace_once(
    text,
    '''                if (!ascii_byte(reconsume_character)) {\n                    // Do not move the existing non-ASCII debt into a new census\n                    // bucket before Unicode tag-name authority is admitted.\n                    return fail_data_tokenizer(\n                        error_,\n                        "HTML Data-tag tokenizer first attribute without separating whitespace is outside admitted v1 subset");\n                }\n                leading_control_error_emitted =\n                    ascii_control_parse_error(reconsume_character);\n''',
    '''                if (!ascii_byte(reconsume_character)) {\n                    const std::size_t scalar_bytes =\n                        detail::html_tokenizer_utf8_scalar_bytes_v1(input_, reconsume_offset);\n                    if (scalar_bytes == 0U) {\n                        return fail_data_tokenizer(\n                            error_,\n                            "HTML Data-tag tokenizer solidus reconsume contains invalid UTF-8 scalar encoding");\n                    }\n                    std::uint32_t scalar = 0U;\n                    if (!decode_utf8_scalar_value(\n                            input_, reconsume_offset, scalar_bytes, &scalar)) {\n                        return fail_data_tokenizer(\n                            error_,\n                            "HTML Data-tag tokenizer solidus reconsume scalar decoding failed");\n                    }\n                    if (unicode_control_parse_error(scalar) || unicode_noncharacter(scalar)) {\n                        return fail_data_tokenizer(\n                            error_,\n                            "HTML Data-tag tokenizer solidus-reconsume Unicode input-error ordering is outside admitted v1 subset");\n                    }\n                    leading_control_error_emitted = false;\n                } else {\n                    leading_control_error_emitted =\n                        ascii_control_parse_error(reconsume_character);\n                }\n''',
    "Unicode solidus reconsume",
)
path.write_text(text, encoding="utf-8")

path = ROOT / "tests/html_tokenizer_data_tags_v1_tests.cpp"
text = path.read_text(encoding="utf-8")
insert_before = '''bool test_token_and_attribute_bounds() {\n'''
new_test = r'''bool test_unicode_tag_and_attribute_scalars() {
    const std::string scalar("\xF4\x80\x80\x80", 4U);
    {
        CollectingSink sink;
        std::string error;
        const std::string input = "<a" + scalar + ">";
        if (!require(tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                     std::string("Unicode tag name: ") + error) ||
            !require(sink.tokens.size() == 1U &&
                         sink.tokens[0].kind == HtmlTokenizerV1TokenKind::StartTag &&
                         sink.tokens[0].name == "a" + scalar,
                     "Unicode tag-name scalar retained") ||
            !require(sink.errors.empty(), "Unicode tag-name scalar has no diagnostics")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        const std::string input = "<" + scalar;
        if (!require(tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                     std::string("Unicode tag-open recovery: ") + error) ||
            !require(sink.tokens.size() == 1U &&
                         sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                         sink.tokens[0].data == input,
                     "Unicode tag-open scalar reconsumed in Data") ||
            !require(sink.errors.size() == 1U &&
                         sink.errors[0].code == "invalid-first-character-of-tag-name" &&
                         sink.errors[0].column == 2U,
                     "Unicode tag-open recovery diagnostic")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        const std::string input = "</" + scalar;
        if (!require(tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                     std::string("Unicode end-tag-open recovery: ") + error) ||
            !require(sink.tokens.size() == 1U &&
                         sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Comment &&
                         sink.tokens[0].data == scalar,
                     "Unicode end-tag-open scalar retained in bogus comment") ||
            !require(sink.errors.size() == 1U &&
                         sink.errors[0].code == "invalid-first-character-of-tag-name" &&
                         sink.errors[0].column == 3U,
                     "Unicode end-tag-open recovery diagnostic")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        const std::string input = "<a " + scalar + "='" + scalar + "' b=" + scalar + ">";
        if (!require(tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                     std::string("Unicode attributes: ") + error) ||
            !require(sink.tokens.size() == 1U && sink.tokens[0].attributes.size() == 2U,
                     "Unicode attribute count") ||
            !require(attribute_is(sink.tokens[0].attributes[0], scalar, scalar),
                     "Unicode attribute name and quoted value retained") ||
            !require(attribute_is(sink.tokens[0].attributes[1], "b", scalar),
                     "Unicode unquoted attribute value retained") ||
            !require(sink.errors.empty(), "Unicode attributes have no diagnostics")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        const std::string input = "<a/" + scalar + ">";
        if (!require(tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                     std::string("Unicode solidus reconsume: ") + error) ||
            !require(sink.tokens.size() == 1U && sink.tokens[0].attributes.size() == 1U &&
                         attribute_is(sink.tokens[0].attributes[0], scalar, ""),
                     "Unicode solidus reconsume becomes attribute") ||
            !require(sink.errors.size() == 1U &&
                         sink.errors[0].code == "unexpected-solidus-in-tag" &&
                         sink.errors[0].column == 4U,
                     "Unicode solidus reconsume diagnostic")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        const std::string input = "<a a=''" + scalar + ">";
        if (!require(tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                     std::string("Unicode missing whitespace: ") + error) ||
            !require(sink.tokens.size() == 1U && sink.tokens[0].attributes.size() == 2U &&
                         attribute_is(sink.tokens[0].attributes[1], scalar, ""),
                     "Unicode missing-whitespace attribute retained") ||
            !require(sink.errors.size() == 1U &&
                         sink.errors[0].code == "missing-whitespace-between-attributes" &&
                         sink.errors[0].column == 8U,
                     "Unicode missing-whitespace diagnostic")) {
            return false;
        }
    }
    return true;
}

'''
text = replace_once(text, insert_before, new_test + insert_before, "Unicode Data-tag focused tests")
text = replace_once(
    text,
    '''        !test_plaintext_start_tag_does_not_fake_tree_builder_feedback() ||\n        !test_admitted_references_and_fail_closed_boundaries() ||\n        !test_token_and_attribute_bounds()) {\n''',
    '''        !test_plaintext_start_tag_does_not_fake_tree_builder_feedback() ||\n        !test_admitted_references_and_fail_closed_boundaries() ||\n        !test_unicode_tag_and_attribute_scalars() ||\n        !test_token_and_attribute_bounds()) {\n''',
    "Unicode Data-tag test registration",
)
path.write_text(text, encoding="utf-8")
print("applied canonical Data-tag Unicode production candidate")
