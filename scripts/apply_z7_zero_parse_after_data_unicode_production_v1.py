#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)

# Data-state tag-open ordering + attribute-value input controls.
path = ROOT / "src/html_tokenizer_data_tags_v1.cpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''            if (ascii_control_parse_error(character) &&
                !emit_parse_error(cursor, "control-character-in-input-stream")) {
                return false;
            }
            if (character == '&') {
''',
    '''            if (ascii_control_parse_error(character)) {
                if (suppressed_control_error_offset_ == cursor) {
                    suppressed_control_error_offset_ =
                        std::numeric_limits<std::size_t>::max();
                } else if (!emit_parse_error(
                               cursor,
                               "control-character-in-input-stream")) {
                    return false;
                }
            }
            if (character == '&') {
''',
    "Data control reconsume suppression",
)
text = replace_once(
    text,
    '''            if (!emit_parse_error(
                    probe,
                    "invalid-first-character-of-tag-name") ||
                !append_character('<')) {
                return false;
            }
            // WHATWG tag-open "anything else" emits '<' then reconsumes the
''',
    '''            const bool leading_control_error =
                ascii_control_parse_error(input_[probe]);
            if (leading_control_error &&
                !emit_parse_error(probe, "control-character-in-input-stream")) {
                return false;
            }
            if (leading_control_error) {
                suppressed_control_error_offset_ = probe;
            }
            if (!emit_parse_error(
                    probe,
                    "invalid-first-character-of-tag-name") ||
                !append_character('<')) {
                return false;
            }
            // WHATWG tag-open "anything else" emits '<' then reconsumes the
''',
    "tag-open input-stream ordering",
)
text = replace_once(
    text,
    '''    std::string* error_{nullptr};
    std::string character_buffer_;
};
''',
    '''    std::string* error_{nullptr};
    std::string character_buffer_;
    std::size_t suppressed_control_error_offset_{
        std::numeric_limits<std::size_t>::max()};
};
''',
    "Data control suppression member",
)
text = replace_once(
    text,
    '''                if (character == '&') {
                    if (!consume_character_reference(
                            cursor,
                            HtmlTokenizerCharacterReferenceV1Context::Attribute,
                            value,
                            "HTML Data-tag tokenizer attribute value")) {
                        return false;
                    }
                    continue;
                }
                if (!append_value_character(value, character)) {
''',
    '''                if (ascii_control_parse_error(character) &&
                    !emit_parse_error(*cursor, "control-character-in-input-stream")) {
                    return false;
                }
                if (character == '&') {
                    if (!consume_character_reference(
                            cursor,
                            HtmlTokenizerCharacterReferenceV1Context::Attribute,
                            value,
                            "HTML Data-tag tokenizer attribute value")) {
                        return false;
                    }
                    continue;
                }
                if (!append_value_character(value, character)) {
''',
    "quoted attribute-value controls",
)
text = replace_once(
    text,
    '''            if (character == '&') {
                if (!consume_character_reference(
                        cursor,
                        HtmlTokenizerCharacterReferenceV1Context::Attribute,
                        value,
                        "HTML Data-tag tokenizer attribute value")) {
                    return false;
                }
                continue;
            }
            if (character == '"' || character == '\'' || character == '<' ||
''',
    '''            if (ascii_control_parse_error(character) &&
                !emit_parse_error(*cursor, "control-character-in-input-stream")) {
                return false;
            }
            if (character == '&') {
                if (!consume_character_reference(
                        cursor,
                        HtmlTokenizerCharacterReferenceV1Context::Attribute,
                        value,
                        "HTML Data-tag tokenizer attribute value")) {
                    return false;
                }
                continue;
            }
            if (character == '"' || character == '\'' || character == '<' ||
''',
    "unquoted attribute-value controls",
)
path.write_text(text, encoding="utf-8")

# Markup input ordering, comment controls, and HTML-content CDATA recovery.
path = ROOT / "src/html_tokenizer_markup_declarations_v1.cpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''        if (ascii_iequals_at(input_, offset_, "<!DOCTYPE")) {
            return consume_doctype();
        }
        return consume_bogus_comment();
''',
    '''        if (ascii_iequals_at(input_, offset_, "<!DOCTYPE")) {
            return consume_doctype();
        }
        if (input_.substr(offset_, 9U) == "<![CDATA[") {
            return consume_cdata_in_html();
        }
        return consume_bogus_comment();
''',
    "CDATA in HTML dispatch",
)
text = replace_once(
    text,
    '''            if (!ascii_byte(value)) {
                return fail_markup(
                    error_,
                    "HTML markup declaration non-ASCII comment authority is not implemented");
            }
            if (data->size() >= config_.maximum_token_bytes) {
''',
    '''            if (!ascii_byte(value)) {
                return fail_markup(
                    error_,
                    "HTML markup declaration non-ASCII comment authority is not implemented");
            }
            if (ascii_control_parse_error(value) &&
                !emit_parse_error(cursor, "control-character-in-input-stream")) {
                return false;
            }
            if (data->size() >= config_.maximum_token_bytes) {
''',
    "normal comment input controls",
)
text = replace_once(
    text,
    '''    bool consume_bogus_comment() {
        const std::size_t data_begin = offset_ + 2U;
        if (!emit_parse_error(data_begin, "incorrectly-opened-comment")) {
            return false;
        }
''',
    '''    bool consume_cdata_in_html() {
        const std::size_t data_begin = offset_ + 2U;
        if (!emit_parse_error(offset_ + 8U, "cdata-in-html-content")) {
            return false;
        }
        std::size_t cursor = data_begin;
        while (cursor < input_.size() && input_[cursor] != '>') {
            ++cursor;
        }
        std::string data;
        if (!collect_comment_data(data_begin, cursor, &data) ||
            !emit_comment_owned(std::move(data))) {
            return false;
        }
        *next_offset_ = cursor < input_.size() ? cursor + 1U : cursor;
        return true;
    }

    bool consume_bogus_comment() {
        const std::size_t data_begin = offset_ + 2U;
        if (data_begin < input_.size() &&
            ascii_control_parse_error(input_[data_begin]) &&
            !emit_parse_error(data_begin, "control-character-in-input-stream")) {
            return false;
        }
        if (!emit_parse_error(data_begin, "incorrectly-opened-comment")) {
            return false;
        }
''',
    "markup ordering and CDATA consumer",
)
path.write_text(text, encoding="utf-8")

# PLAINTEXT input controls.
path = ROOT / "src/html_tokenizer_token_stream_v1.cpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''                if (!append_character(input_[cursor])) {
                    return false;
                }
                ++cursor;
                break;
            case ActiveState::Rcdata:
''',
    '''                if (input_control_parse_error(input_[cursor]) &&
                    !emit_parse_error(cursor, "control-character-in-input-stream")) {
                    return false;
                }
                if (!append_character(input_[cursor])) {
                    return false;
                }
                ++cursor;
                break;
            case ActiveState::Rcdata:
''',
    "PLAINTEXT input controls",
)
path.write_text(text, encoding="utf-8")

# Script-data input controls, with same-offset reconsume suppression.
path = ROOT / "src/html_tokenizer_script_data_v1.cpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''bool ascii_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' ||
        value == '\r' || value == '\f';
}
''',
    '''bool ascii_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' ||
        value == '\r' || value == '\f';
}

bool ascii_control_parse_error(char value) noexcept {
    const auto byte = static_cast<unsigned char>(value);
    return (byte >= 0x01U && byte <= 0x08U) || byte == 0x0BU ||
        (byte >= 0x0EU && byte <= 0x1FU) || byte == 0x7FU;
}
''',
    "Script control helper",
)
text = replace_once(
    text,
    '''        while (cursor < input_.size() && !done_) {
            if (!ascii_byte(input_[cursor])) {
''',
    '''        while (cursor < input_.size() && !done_) {
            if (!ascii_byte(input_[cursor])) {
''',
    "Script run loop anchor",
)
text = replace_once(
    text,
    '''                    "HTML Script-data non-ASCII preprocessing/location authority is not implemented");
            }
            if (!consume_state(&cursor)) {
''',
    '''                    "HTML Script-data non-ASCII preprocessing/location authority is not implemented");
            }
            if (ascii_control_parse_error(input_[cursor]) &&
                observed_control_error_offset_ != cursor) {
                if (!emit_parse_error(cursor, "control-character-in-input-stream")) {
                    stats_->bytes_consumed = static_cast<std::uint64_t>(cursor);
                    return false;
                }
                observed_control_error_offset_ = cursor;
            }
            if (!consume_state(&cursor)) {
''',
    "Script input control observation",
)
text = replace_once(
    text,
    '''    bool done_{false};
};
''',
    '''    bool done_{false};
    std::size_t observed_control_error_offset_{
        std::numeric_limits<std::size_t>::max()};
};
''',
    "Script control suppression member",
)
path.write_text(text, encoding="utf-8")

print("applied zero-residual-parse production candidate")
