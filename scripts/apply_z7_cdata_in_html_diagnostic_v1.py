#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
path = ROOT / "src/html_tokenizer_markup_declarations_v1.cpp"
text = path.read_text(encoding="utf-8")
old = '''        if (input_.substr(offset_, 4U) == "<!--") {
            return consume_comment();
        }
        if (ascii_iequals_at(input_, offset_, "<!DOCTYPE")) {
            return consume_doctype();
        }
        return consume_bogus_comment();
'''
new = '''        if (input_.substr(offset_, 4U) == "<!--") {
            return consume_comment();
        }
        if (ascii_iequals_at(input_, offset_, "<!DOCTYPE")) {
            return consume_doctype();
        }
        if (input_.substr(offset_, 9U) == "<![CDATA[") {
            return consume_cdata_in_html_comment();
        }
        return consume_bogus_comment();
'''
if text.count(old) != 1:
    raise SystemExit(f"markup dispatch anchor count={text.count(old)}")
text = text.replace(old, new, 1)
anchor = '''    bool consume_bogus_comment() {
'''
method = '''    bool consume_cdata_in_html_comment() {
        if (!emit_parse_error(offset_ + 8U, "cdata-in-html-content")) {
            return false;
        }
        const std::size_t data_begin = offset_ + 2U;
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
        if (!emit_comment(input_.substr(data_begin, cursor - data_begin))) {
            return false;
        }
        *next_offset_ = cursor < input_.size() ? cursor + 1U : cursor;
        return true;
    }

'''
if text.count(anchor) != 1:
    raise SystemExit(f"bogus comment anchor count={text.count(anchor)}")
text = text.replace(anchor, method + anchor, 1)
path.write_text(text, encoding="utf-8")
print("applied CDATA-in-HTML diagnostic candidate")
