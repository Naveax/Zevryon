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
text = replace_once(
    text,
    '''            if (input_[cursor] == '\\0') {\n                return fail_markup(\n                    error_,\n                    "HTML markup declaration input preprocessing/NUL replacement is not implemented");\n            }\n''',
    '''''',
    "bogus-comment NUL prevalidation removal",
)
path.write_text(text, encoding="utf-8")

path = ROOT / "tests/html_tokenizer_markup_declarations_v1_tests.cpp"
text = path.read_text(encoding="utf-8")
insert_before = '''bool test_fail_closed_boundaries() {\n'''
new_test = r'''bool test_bogus_comment_nul_semantics() {
    const std::string replacement("\xEF\xBF\xBD", 3U);
    {
        std::string input = "<!";
        input.push_back('\0');
        if (!run_comment_case(
                "bogus comment NUL",
                input,
                replacement,
                {
                    ExpectedError{"incorrectly-opened-comment", 1U, 3U},
                    ExpectedError{"unexpected-null-character", 1U, 3U},
                })) {
            return false;
        }
    }
    {
        std::string input = "<! ";
        input.push_back('\0');
        std::string expected = " ";
        expected += replacement;
        if (!run_comment_case(
                "bogus comment spaced NUL",
                input,
                expected,
                {
                    ExpectedError{"incorrectly-opened-comment", 1U, 3U},
                    ExpectedError{"unexpected-null-character", 1U, 4U},
                })) {
            return false;
        }
    }
    {
        std::string input = "<!doc";
        input.push_back('\0');
        std::string expected = "doc";
        expected += replacement;
        if (!run_comment_case(
                "bogus comment trailing NUL",
                input,
                expected,
                {
                    ExpectedError{"incorrectly-opened-comment", 1U, 3U},
                    ExpectedError{"unexpected-null-character", 1U, 6U},
                })) {
            return false;
        }
    }
    return true;
}

'''
text = replace_once(text, insert_before, new_test + insert_before, "bogus-comment NUL focused tests")
text = replace_once(
    text,
    '''        !test_pinned_test1_comments() ||\n        !test_nonzero_offset_preserves_global_location() ||\n        !test_fail_closed_boundaries()) {\n''',
    '''        !test_pinned_test1_comments() ||\n        !test_nonzero_offset_preserves_global_location() ||\n        !test_bogus_comment_nul_semantics() ||\n        !test_fail_closed_boundaries()) {\n''',
    "bogus-comment NUL test registration",
)
path.write_text(text, encoding="utf-8")

path = ROOT / "scripts/z7_html5lib_tokenizer_full_corpus_census_v1.py"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''    # Ordinary Data NUL, the complete pinned DOCTYPE-NUL family, and\n    # normal HTML-comment NUL are admitted. Tag/bogus-comment NUL remains fail closed.\n''',
    '''    # Ordinary Data NUL, the complete pinned DOCTYPE-NUL family, normal\n    # HTML-comment NUL, tag-family NUL, and proven markup-declaration bogus-comment\n    # NUL are admitted.\n''',
    "NUL authority comment",
)
text = replace_once(
    text,
    '''            input_text.startswith("<!--") or\n            (input_text.startswith("<") and not input_text.startswith("<!"))\n''',
    '''            input_text.startswith("<!--") or\n            (input_text.startswith("<") and not input_text.startswith("<!")) or\n            (\n                input_text.startswith("<!") and\n                not input_text.startswith("<!--") and\n                input_text[:9].lower() != "<!doctype"\n            )\n''',
    "bogus-comment NUL census gate",
)
path.write_text(text, encoding="utf-8")
print("applied residual markup NUL production candidate")
