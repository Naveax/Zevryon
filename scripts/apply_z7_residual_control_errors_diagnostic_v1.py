#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)

# PLAINTEXT control-character input-stream errors.
path = ROOT / "src/html_tokenizer_token_stream_v1.cpp"
text = path.read_text(encoding="utf-8")
old = '''                if (!append_character(input_[cursor])) {
                    return false;
                }
                ++cursor;
                break;
            case ActiveState::Rcdata:
'''
new = '''                if (input_control_parse_error(input_[cursor]) &&
                    !emit_parse_error(cursor, "control-character-in-input-stream")) {
                    return false;
                }
                if (!append_character(input_[cursor])) {
                    return false;
                }
                ++cursor;
                break;
            case ActiveState::Rcdata:
'''
text = replace_once(text, old, new, "PLAINTEXT control error")
path.write_text(text, encoding="utf-8")

# Script-data control-character input-stream errors.
path = ROOT / "src/html_tokenizer_script_data_v1.cpp"
text = path.read_text(encoding="utf-8")
anchor = '''bool ascii_space(char value) noexcept {
    return value == ' ' || value == '\\t' || value == '\\n' ||
        value == '\\r' || value == '\\f';
}
'''
insert = anchor + '''\nbool ascii_control_parse_error(char value) noexcept {\n    const auto byte = static_cast<unsigned char>(value);\n    return (byte >= 0x01U && byte <= 0x08U) || byte == 0x0BU ||\n        (byte >= 0x0EU && byte <= 0x1FU) || byte == 0x7FU;\n}\n'''
text = replace_once(text, anchor, insert, "Script control predicate")
old = '''            if (!consume_state(&cursor)) {
                stats_->bytes_consumed = static_cast<std::uint64_t>(cursor);
                return false;
            }
'''
new = '''            if (ascii_control_parse_error(input_[cursor]) &&
                !emit_parse_error(cursor, "control-character-in-input-stream")) {
                stats_->bytes_consumed = static_cast<std::uint64_t>(cursor);
                return false;
            }
            if (!consume_state(&cursor)) {
                stats_->bytes_consumed = static_cast<std::uint64_t>(cursor);
                return false;
            }
'''
text = replace_once(text, old, new, "Script run control error")
path.write_text(text, encoding="utf-8")

# Attribute-value control errors.
path = ROOT / "src/html_tokenizer_data_tags_v1.cpp"
text = path.read_text(encoding="utf-8")
old = '''                if (!ascii_byte(character)) {
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer non-ASCII attribute-value authority is not implemented");
                }
                if (character == '&') {
'''
new = '''                if (!ascii_byte(character)) {
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer non-ASCII attribute-value authority is not implemented");
                }
                if (ascii_control_parse_error(character) &&
                    !emit_parse_error(*cursor, "control-character-in-input-stream")) {
                    return false;
                }
                if (character == '&') {
'''
text = replace_once(text, old, new, "quoted attribute control error")
old = '''            if (!ascii_byte(character)) {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer non-ASCII attribute-value authority is not implemented");
            }
            if (character == '&') {
'''
new = '''            if (!ascii_byte(character)) {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer non-ASCII attribute-value authority is not implemented");
            }
            if (ascii_control_parse_error(character) &&
                !emit_parse_error(*cursor, "control-character-in-input-stream")) {
                return false;
            }
            if (character == '&') {
'''
text = replace_once(text, old, new, "unquoted attribute control error")
path.write_text(text, encoding="utf-8")

# Markup comments and bogus comments.
path = ROOT / "src/html_tokenizer_markup_declarations_v1.cpp"
text = path.read_text(encoding="utf-8")
anchor = '''    bool consume_bogus_comment() {
'''
helper = '''    bool emit_control_errors_between(std::size_t begin, std::size_t end) {
        const std::size_t bounded_end = std::min(end, input_.size());
        for (std::size_t index = std::min(begin, bounded_end); index < bounded_end; ++index) {
            if (ascii_control_parse_error(input_[index]) &&
                !emit_parse_error(index, "control-character-in-input-stream")) {
                return false;
            }
        }
        return true;
    }

'''
text = replace_once(text, anchor, helper + anchor, "markup control helper")
old = '''            if (!ascii_byte(input_[cursor])) {
                return fail_markup(
                    error_,
                    "HTML markup declaration non-ASCII bogus-comment authority is not implemented");
            }
            ++cursor;
'''
new = '''            if (!ascii_byte(input_[cursor])) {
                return fail_markup(
                    error_,
                    "HTML markup declaration non-ASCII bogus-comment authority is not implemented");
            }
            if (ascii_control_parse_error(input_[cursor]) &&
                !emit_parse_error(cursor, "control-character-in-input-stream")) {
                return false;
            }
            ++cursor;
'''
text = replace_once(text, old, new, "bogus comment control error")
old = '''        const std::size_t close = input_.find("-->", data_begin);
        if (close != std::string_view::npos) {
            const std::size_t nested = input_.find("<!--", data_begin);
            if (nested != std::string_view::npos && nested + 4U <= close) {
                if (!emit_parse_error(nested + 4U, "nested-comment")) {
                    return false;
                }
            }
            const std::string_view data = input_.substr(data_begin, close - data_begin);
'''
new = '''        const std::size_t close = input_.find("-->", data_begin);
        if (close != std::string_view::npos) {
            const std::size_t nested = input_.find("<!--", data_begin);
            if (nested != std::string_view::npos && nested + 4U <= close) {
                if (!emit_control_errors_between(data_begin, nested + 4U) ||
                    !emit_parse_error(nested + 4U, "nested-comment") ||
                    !emit_control_errors_between(nested + 4U, close)) {
                    return false;
                }
            } else if (!emit_control_errors_between(data_begin, close)) {
                return false;
            }
            const std::string_view data = input_.substr(data_begin, close - data_begin);
'''
text = replace_once(text, old, new, "closed comment control errors")
old = '''        const std::string_view data = input_.substr(data_begin, data_end - data_begin);
        if (!emit_parse_error(input_.size(), "eof-in-comment") ||
            !emit_comment(data)) {
'''
new = '''        const std::string_view data = input_.substr(data_begin, data_end - data_begin);
        if (!emit_control_errors_between(data_begin, input_.size()) ||
            !emit_parse_error(input_.size(), "eof-in-comment") ||
            !emit_comment(data)) {
'''
text = replace_once(text, old, new, "EOF comment control errors")
path.write_text(text, encoding="utf-8")
print("applied residual control-character diagnostic patch")
