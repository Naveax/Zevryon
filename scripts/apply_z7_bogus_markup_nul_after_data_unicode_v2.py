#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)

path = ROOT / "src/html_tokenizer_markup_declarations_v1.cpp"
text = path.read_text(encoding="utf-8")
old = '''    bool consume_bogus_comment() {
        const std::size_t data_begin = offset_ + 2U;
        if (!emit_parse_error(data_begin, "incorrectly-opened-comment")) {
            return false;
        }
        std::size_t cursor = data_begin;
        while (cursor < input_.size() && input_[cursor] != '>') {
            if (input_[cursor] == '\\0') {
                return fail_markup(
                    error_,
                    "HTML markup declaration input preprocessing/NUL replacement is not implemented");
            }
            if (!ascii_byte(input_[cursor])) {
                return fail_markup(
                    error_,
                    "HTML markup declaration non-ASCII bogus-comment authority is not implemented");
            }
            ++cursor;
        }
        const std::string_view data = input_.substr(data_begin, cursor - data_begin);
        if (!emit_comment(data)) {
            return false;
        }
        *next_offset_ = cursor < input_.size() ? cursor + 1U : cursor;
        return true;
    }
'''
new = '''    bool consume_bogus_comment() {
        const std::size_t data_begin = offset_ + 2U;
        if (!emit_parse_error(data_begin, "incorrectly-opened-comment")) {
            return false;
        }
        std::size_t cursor = data_begin;
        while (cursor < input_.size() && input_[cursor] != '>') {
            if (!ascii_byte(input_[cursor]) && input_[cursor] != '\\0') {
                return fail_markup(
                    error_,
                    "HTML markup declaration non-ASCII bogus-comment authority is not implemented");
            }
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
'''
text = replace_once(text, old, new, "bogus markup comment NUL semantics")
path.write_text(text, encoding="utf-8")

path = ROOT / "scripts/z7_html5lib_tokenizer_full_corpus_census_v1.py"
text = path.read_text(encoding="utf-8")
old = '''        elif state_name == "Data state" and (
            "<" not in input_text or
            input_text[:9].lower() == "<!doctype" or
            input_text.startswith("<!--")
        ):
            pass
'''
new = '''        elif state_name == "Data state" and (
            "<" not in input_text or input_text.startswith("<!")
        ):
            pass
'''
text = replace_once(text, old, new, "bogus markup NUL diagnostic classifier")
path.write_text(text, encoding="utf-8")
print("applied bogus-markup NUL diagnostic after Data Unicode")
