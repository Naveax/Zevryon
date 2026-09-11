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
        } else {
            const unsigned char byte = static_cast<unsigned char>(input[index]);
            if ((byte & 0xC0U) == 0x80U) {
                continue;
            }
            result.column += (byte & 0xF8U) == 0xF0U ? 2U : 1U;
        }
""",
    "UTF-16-code-unit source columns",
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
        "non-BMP UTF-8 DOCTYPE name EOF UTF-16 column",
        eof_name,
        eof_expected,
        true,
        {ExpectedError{"eof-in-doctype", 1U, 16U}});
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


doc = ROOT / "docs/Z7_HTML_TOKENIZER_MARKUP_DECLARATIONS_V1.md"
text = doc.read_text(encoding="utf-8")
text = replace_once(
    text,
    """- bounded ASCII DOCTYPE-name handling with ASCII uppercase normalization;
- missing-whitespace-before-name recovery for an otherwise representable ASCII name;
""",
    """- bounded UTF-8 DOCTYPE-name handling with ASCII uppercase normalization;
- missing-whitespace-before-name recovery for representable ASCII and UTF-8 names;
- html5lib-compatible one-based parse-error columns measured in UTF-16 code units for valid UTF-8 input, including non-BMP scalars;
""",
    "document Unicode DOCTYPE-name surface",
)
text = replace_once(
    text,
    """- non-ASCII DOCTYPE-name or identifier authority where preprocessing/location semantics are not yet admitted;
""",
    """- non-ASCII DOCTYPE public/system identifier authority outside the admitted name states;
""",
    "document remaining DOCTYPE Unicode boundary",
)
text = replace_once(
    text,
    """For unsupported non-ASCII DOCTYPE inputs the production boundary retains the pre-existing census failure class instead of silently moving an execution from one unsupported reason bucket to another. This matters because the full-corpus `no-regression` authority checks every fixture and every failure/unsupported reason independently, not just global totals.
""",
    """Non-ASCII bytes are admitted only while the DOCTYPE machine is entering or consuming the name (`AfterKeyword`, `BeforeName`, `Name`). Public/system identifier and other non-ASCII recovery states remain fail closed. Source columns follow the pinned html5lib fixture convention: BMP UTF-8 scalars count as one UTF-16 code unit and non-BMP scalars count as two. This bounded distinction prevents unsupported executions from being converted into parse-error-stream regressions.
""",
    "document bounded Unicode authority",
)
text = replace_once(
    text,
    """- nullable missing-name recovery at `>` and EOF;
- retained non-ASCII and NUL fail-closed boundaries.
""",
    """- nullable missing-name recovery at `>` and EOF;
- UTF-8 DOCTYPE names with and without separating whitespace;
- non-BMP DOCTYPE-name EOF diagnostics using UTF-16-code-unit columns;
- retained non-ASCII identifier/comment and NUL fail-closed boundaries.
""",
    "document focused Unicode regressions",
)
doc.write_text(text, encoding="utf-8")

print("applied bounded Unicode DOCTYPE-name production patch")
