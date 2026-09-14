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
    '''            if (character == '/' &&
                *cursor + 1U < input_.size() &&
                input_[*cursor + 1U] == '>') {
                break;
            }
''',
    '',
    "unquoted slash data",
)
path.write_text(text, encoding="utf-8")

path = ROOT / "src/html_tokenizer_markup_declarations_v1.cpp"
text = path.read_text(encoding="utf-8")
old = '''        const std::size_t close = input_.find("-->", data_begin);
        if (close != std::string_view::npos) {
            const std::size_t nested = input_.find("<!--", data_begin);
            if (nested != std::string_view::npos && nested + 4U <= close) {
                if (!emit_parse_error(nested + 4U, "nested-comment")) {
                    return false;
                }
            }
            std::string data;
            if (!collect_comment_data(data_begin, close, &data) ||
                !emit_comment_owned(std::move(data))) {
                return false;
            }
            *next_offset_ = close + 3U;
            return true;
        }

        std::size_t data_end = input_.size();
        if (data_end >= data_begin + 2U &&
            input_[data_end - 2U] == '-' && input_[data_end - 1U] == '-') {
            data_end -= 2U;
        }
        std::string data;
        if (!collect_comment_data(data_begin, data_end, &data) ||
'''
new = '''        const std::size_t close = input_.find("-->", data_begin);
        const std::size_t bang_close = input_.find("--!>", data_begin);
        if (bang_close != std::string_view::npos &&
            (close == std::string_view::npos || bang_close < close)) {
            const std::size_t nested = input_.find("<!--", data_begin);
            if (nested != std::string_view::npos && nested + 4U <= bang_close) {
                if (!emit_parse_error(nested + 4U, "nested-comment")) {
                    return false;
                }
            }
            std::string data;
            if (!collect_comment_data(data_begin, bang_close, &data) ||
                !emit_parse_error(bang_close + 3U, "incorrectly-closed-comment") ||
                !emit_comment_owned(std::move(data))) {
                return false;
            }
            *next_offset_ = bang_close + 4U;
            return true;
        }
        if (close != std::string_view::npos) {
            const std::size_t nested = input_.find("<!--", data_begin);
            if (nested != std::string_view::npos && nested + 4U <= close) {
                if (!emit_parse_error(nested + 4U, "nested-comment")) {
                    return false;
                }
            }
            std::string data;
            if (!collect_comment_data(data_begin, close, &data) ||
                !emit_comment_owned(std::move(data))) {
                return false;
            }
            *next_offset_ = close + 3U;
            return true;
        }

        std::size_t data_end = input_.size();
        if (data_end >= data_begin + 3U &&
            input_[data_end - 3U] == '-' &&
            input_[data_end - 2U] == '-' &&
            input_[data_end - 1U] == '!') {
            data_end -= 3U;
        } else {
            std::size_t trailing_dashes = 0U;
            while (data_end > data_begin && trailing_dashes < 2U &&
                   input_[data_end - 1U] == '-') {
                --data_end;
                ++trailing_dashes;
            }
        }
        std::string data;
        if (!collect_comment_data(data_begin, data_end, &data) ||
'''
text = replace_once(text, old, new, "comment EOF/end-bang recovery")
path.write_text(text, encoding="utf-8")
print("applied stacked residual token-stream diagnostic fixes")
