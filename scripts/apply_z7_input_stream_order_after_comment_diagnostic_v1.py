#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


# The Unicode candidate is applied first by the workflow. Preserve the single
# input-stream control error when an invalid tag-open byte is reconsumed in Data.
path = ROOT / "src/html_tokenizer_data_tags_v1.cpp"
text = path.read_text(encoding="utf-8")
old = '''            if (ascii_control_parse_error(character) &&
                !emit_parse_error(cursor, "control-character-in-input-stream")) {
                return false;
            }
            if (character == '&') {
'''
new = '''            if (ascii_control_parse_error(character)) {
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
'''
text = replace_once(text, old, new, "Data control reconsume suppression")
old = '''            if (!emit_parse_error(
                    probe,
                    "invalid-first-character-of-tag-name") ||
                !append_character('<')) {
                return false;
            }
            // WHATWG tag-open "anything else" emits '<' then reconsumes the
'''
new = '''            const bool leading_control_error =
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
'''
text = replace_once(text, old, new, "tag-open input-stream ordering")
old = '''    std::string* error_{nullptr};
    std::string character_buffer_;
};
'''
new = '''    std::string* error_{nullptr};
    std::string character_buffer_;
    std::size_t suppressed_control_error_offset_{
        std::numeric_limits<std::size_t>::max()};
};
'''
text = replace_once(text, old, new, "Data control suppression member")
path.write_text(text, encoding="utf-8")


# Markup declaration open consumes the first bogus-comment byte only once, so
# its input-stream control error must precede incorrectly-opened-comment.
path = ROOT / "src/html_tokenizer_markup_declarations_v1.cpp"
text = path.read_text(encoding="utf-8")
old = '''    bool consume_bogus_comment() {
        const std::size_t data_begin = offset_ + 2U;
        if (!emit_parse_error(data_begin, "incorrectly-opened-comment")) {
            return false;
        }
'''
new = '''    bool consume_bogus_comment() {
        const std::size_t data_begin = offset_ + 2U;
        if (data_begin < input_.size() &&
            ascii_control_parse_error(input_[data_begin]) &&
            !emit_parse_error(data_begin, "control-character-in-input-stream")) {
            return false;
        }
        if (!emit_parse_error(data_begin, "incorrectly-opened-comment")) {
            return false;
        }
'''
text = replace_once(text, old, new, "markup-open input-stream ordering")
path.write_text(text, encoding="utf-8")

print("applied input-stream ordering diagnostic candidate")
