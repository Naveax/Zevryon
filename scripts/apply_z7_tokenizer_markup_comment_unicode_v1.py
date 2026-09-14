#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


def replace_in_region(text: str, start_marker: str, end_marker: str, old: str, new: str, label: str) -> str:
    start = text.index(start_marker)
    end = text.index(end_marker, start + len(start_marker))
    region = text[start:end]
    count = region.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one region-local anchor, found {count}")
    region = region.replace(old, new, 1)
    return text[:start] + region + text[end:]


path = ROOT / "src/html_tokenizer_markup_declarations_v1.cpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''#include "html_tokenizer_markup_declarations_v1.hpp"\n\n''',
    '''#include "html_tokenizer_markup_declarations_v1.hpp"\n\n#include "html_tokenizer_utf8_v1.hpp"\n\n''',
    "UTF-8 helper include",
)
text = replace_once(
    text,
    '''bool validate_ascii_span(\n    std::string_view value,\n    std::string* error,\n    std::string_view label) {\n    for (char character : value) {\n        if (character == '\\0') {\n            return fail_markup(\n                error,\n                "HTML markup declaration input preprocessing/NUL replacement is not implemented");\n        }\n        if (!ascii_byte(character)) {\n            return fail_markup(\n                error,\n                std::string("HTML markup declaration non-ASCII ") +\n                    std::string(label) + " authority is not implemented");\n        }\n    }\n    return true;\n}\n''',
    '''bool validate_comment_span(\n    std::string_view value,\n    std::string* error,\n    std::string_view label) {\n    std::size_t cursor = 0U;\n    while (cursor < value.size()) {\n        const char character = value[cursor];\n        if (character == '\\0') {\n            return fail_markup(\n                error,\n                "HTML markup declaration input preprocessing/NUL replacement is not implemented");\n        }\n        if (ascii_byte(character)) {\n            ++cursor;\n            continue;\n        }\n        const std::size_t scalar_bytes =\n            detail::html_tokenizer_utf8_scalar_bytes_v1(value, cursor);\n        if (scalar_bytes == 0U) {\n            return fail_markup(\n                error,\n                std::string("HTML markup declaration invalid UTF-8 ") +\n                    std::string(label) + " scalar encoding");\n        }\n        cursor += scalar_bytes;\n    }\n    return true;\n}\n''',
    "comment UTF-8 validator",
)
text = replace_once(
    text,
    '''        if (!validate_ascii_span(data, error_, "comment")) {\n''',
    '''        if (!validate_comment_span(data, error_, "comment")) {\n''',
    "comment validator call",
)
text = replace_in_region(
    text,
    '''    bool consume_bogus_comment() {\n''',
    '''};\n\n} // namespace\n''',
    '''            if (!ascii_byte(input_[cursor])) {\n                return fail_markup(\n                    error_,\n                    "HTML markup declaration non-ASCII bogus-comment authority is not implemented");\n            }\n            ++cursor;\n''',
    '''            if (!ascii_byte(input_[cursor])) {\n                const std::size_t scalar_bytes =\n                    detail::html_tokenizer_utf8_scalar_bytes_v1(input_, cursor);\n                if (scalar_bytes == 0U) {\n                    return fail_markup(\n                        error_,\n                        "HTML markup declaration bogus comment contains invalid UTF-8 scalar encoding");\n                }\n                cursor += scalar_bytes;\n                continue;\n            }\n            ++cursor;\n''',
    "bogus comment Unicode scan",
)
path.write_text(text, encoding="utf-8")


path = ROOT / "tests/html_tokenizer_markup_declarations_v1_tests.cpp"
text = path.read_text(encoding="utf-8")
insert_before = '''bool test_fail_closed_boundaries() {\n'''
new_test = r'''bool test_comment_unicode_scalars() {
    const std::string scalar("\xF0\x90\x80\x80", 4U);
    {
        std::string input = "<!--";
        input += scalar;
        input += "-->";
        if (!run_comment_case(
                "non-BMP scalar in normal comment",
                input,
                scalar)) {
            return false;
        }
    }
    {
        std::string input = "<!";
        input += scalar;
        input.push_back('>');
        if (!run_comment_case(
                "non-BMP scalar in bogus comment",
                input,
                scalar,
                {ExpectedError{"incorrectly-opened-comment", 1U, 3U}})) {
            return false;
        }
    }
    {
        std::string input = "<!--";
        input += scalar;
        if (!run_comment_case(
                "non-BMP scalar before comment EOF",
                input,
                scalar,
                {ExpectedError{"eof-in-comment", 1U, 7U}})) {
            return false;
        }
    }
    return true;
}

'''
text = replace_once(text, insert_before, new_test + insert_before, "comment Unicode focused tests")
text = replace_once(
    text,
    '''        !test_pinned_test1_comments() ||\n        !test_nonzero_offset_preserves_global_location() ||\n        !test_fail_closed_boundaries()) {\n''',
    '''        !test_pinned_test1_comments() ||\n        !test_nonzero_offset_preserves_global_location() ||\n        !test_comment_unicode_scalars() ||\n        !test_fail_closed_boundaries()) {\n''',
    "comment Unicode test registration",
)
path.write_text(text, encoding="utf-8")

print("applied markup comment Unicode diagnostic patch")
