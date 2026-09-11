#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


source = ROOT / "src/html_tokenizer_markup_declarations_v1.cpp"
text = source.read_text(encoding="utf-8")
text = replace_once(
    text,
    """        if (input[index] == '\\n') {
            ++result.line;
            result.column = 1U;
        } else {
            ++result.column;
        }
""",
    """        if (input[index] == '\\n') {
            ++result.line;
            result.column = 1U;
        } else if ((static_cast<unsigned char>(input[index]) & 0xC0U) != 0x80U) {
            ++result.column;
        }
""",
    "UTF-8 scalar-aware source columns",
)
text = replace_once(
    text,
    """        if (!ascii_byte(value)) {
            return fail_unsupported_doctype_ascii_boundary(
                original_missing_whitespace_before_name,
                state);
        }
""",
    """        if (!ascii_byte(value)) {
            if (state == DoctypeState::AfterKeyword ||
                state == DoctypeState::BeforeName ||
                state == DoctypeState::Name) {
                return true;
            }
            return fail_unsupported_doctype_ascii_boundary(
                original_missing_whitespace_before_name,
                state);
        }
""",
    "Unicode DOCTYPE-name observer admission",
)
source.write_text(text, encoding="utf-8")


tests = ROOT / "tests/html_tokenizer_markup_declarations_v1_tests.cpp"
text = tests.read_text(encoding="utf-8")
new_test = r'''bool test_utf8_doctype_name_bytes() {
    std::string spaced = "<!DOCTYPE caf";
    spaced.append("\xC3\xA9", 2U);
    spaced.push_back('>');
    std::string expected = "caf";
    expected.append("\xC3\xA9", 2U);
    if (!run_doctype_case(
            "UTF-8 scalar in DOCTYPE name",
            spaced,
            expected,
            false)) {
        return false;
    }

    std::string adjacent = "<!DOCTYPE";
    adjacent.append("\xC3\xA9", 2U);
    adjacent.push_back('>');
    const std::string adjacent_expected("\xC3\xA9", 2U);
    if (!run_doctype_case(
            "UTF-8 scalar starts DOCTYPE name without whitespace",
            adjacent,
            adjacent_expected,
            false,
            {ExpectedError{"missing-whitespace-before-doctype-name", 1U, 10U}})) {
        return false;
    }

    std::string eof_name = "<!DOCTYPE caf";
    eof_name.append("\xF0\x90\x80\x80", 4U);
    std::string eof_expected = "caf";
    eof_expected.append("\xF0\x90\x80\x80", 4U);
    return run_doctype_case(
        "non-BMP UTF-8 DOCTYPE name EOF scalar column",
        eof_name,
        eof_expected,
        true,
        {ExpectedError{"eof-in-doctype", 1U, 15U}});
}

'''
text = replace_once(
    text,
    """bool test_bounded_ascii_doctype_recovery() {
""",
    new_test + """bool test_bounded_ascii_doctype_recovery() {
""",
    "insert Unicode DOCTYPE-name regressions",
)
text = replace_once(
    text,
    """        !test_missing_doctype_name_recovery() ||
        !test_bounded_ascii_doctype_recovery() ||
""",
    """        !test_missing_doctype_name_recovery() ||
        !test_utf8_doctype_name_bytes() ||
        !test_bounded_ascii_doctype_recovery() ||
""",
    "register Unicode DOCTYPE-name regressions",
)
tests.write_text(text, encoding="utf-8")

print("applied bounded Unicode DOCTYPE-name + scalar-column diagnostic patch")
