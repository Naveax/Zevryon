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
    """        if (!ascii_byte(value)) {
            if (state == DoctypeState::AfterKeyword ||
                state == DoctypeState::BeforeName ||
                state == DoctypeState::Name ||
                state == DoctypeState::PublicIdentifierDoubleQuoted ||
                state == DoctypeState::PublicIdentifierSingleQuoted ||
                state == DoctypeState::SystemIdentifierDoubleQuoted ||
                state == DoctypeState::SystemIdentifierSingleQuoted) {
                return true;
            }
            return fail_unsupported_doctype_ascii_boundary(
                original_missing_whitespace_before_name,
                state);
        }
""",
    "quoted Unicode DOCTYPE identifier observer admission",
)
source.write_text(text, encoding="utf-8")


tests = ROOT / "tests/html_tokenizer_markup_declarations_v1_tests.cpp"
text = tests.read_text(encoding="utf-8")
new_test = r'''bool test_utf8_doctype_quoted_identifiers() {
    const std::string scalar("\xF0\x90\x80\x80", 4U);

    std::string public_input = "<!DOCTYPEa PUBLIC\"";
    public_input += scalar;
    if (!run_doctype_full_case(
            "non-BMP UTF-8 public identifier EOF",
            public_input,
            "a",
            true,
            scalar,
            false,
            {},
            true,
            {
                ExpectedError{"missing-whitespace-before-doctype-name", 1U, 10U},
                ExpectedError{"missing-whitespace-after-doctype-public-keyword", 1U, 18U},
                ExpectedError{"eof-in-doctype", 1U, 21U},
            })) {
        return false;
    }

    std::string system_input = "<!DOCTYPE a SYSTEM \"";
    system_input += scalar;
    system_input += "\">";
    return run_doctype_full_case(
        "non-BMP UTF-8 system identifier closed",
        system_input,
        "a",
        false,
        {},
        true,
        scalar,
        false);
}

'''
text = replace_once(
    text,
    """bool test_bounded_ascii_doctype_recovery() {
""",
    new_test + """bool test_bounded_ascii_doctype_recovery() {
""",
    "insert quoted Unicode identifier regressions",
)
text = replace_once(
    text,
    """        !test_utf8_doctype_name_bytes() ||
        !test_bounded_ascii_doctype_recovery() ||
""",
    """        !test_utf8_doctype_name_bytes() ||
        !test_utf8_doctype_quoted_identifiers() ||
        !test_bounded_ascii_doctype_recovery() ||
""",
    "register quoted Unicode identifier regressions",
)
text = replace_once(
    text,
    """        std::string input = "<!DOCTYPE a PUBLIC \\\"";
        input.append("\\xC2\\xAC", 2U);
        input += "\\\">";
""",
    """        std::string input = "<!DOCTYPE a PUBLIC ";
        input.append("\\xC2\\xAC", 2U);
        input += ">";
""",
    "retain unquoted non-ASCII DOCTYPE transition fail-closed regression",
)
tests.write_text(text, encoding="utf-8")


doc = ROOT / "docs/Z7_HTML_TOKENIZER_MARKUP_DECLARATIONS_V1.md"
text = doc.read_text(encoding="utf-8")
text = replace_once(
    text,
    "- double- and single-quoted public/system identifiers with explicit null-vs-empty presence flags;",
    "- double- and single-quoted public/system identifiers with explicit null-vs-empty presence flags, including valid UTF-8 payload bytes;",
    "document quoted UTF-8 identifier admission",
)
text = replace_once(
    text,
    "- non-ASCII DOCTYPE public/system identifier authority outside the admitted name states;",
    "- non-ASCII DOCTYPE PUBLIC/SYSTEM transition and malformed-recovery authority outside quoted identifier payload states;",
    "document retained PUBLIC/SYSTEM transition boundary",
)
text = replace_once(
    text,
    "Non-ASCII bytes are admitted only while the DOCTYPE machine is entering or consuming the name (`AfterKeyword`, `BeforeName`, `Name`). Public/system identifier and other non-ASCII recovery states remain fail closed. Source columns follow the pinned html5lib fixture convention: BMP UTF-8 scalars count as one UTF-16 code unit and non-BMP scalars count as two. This bounded distinction prevents unsupported executions from being converted into parse-error-stream regressions.",
    "Non-ASCII bytes are admitted while the DOCTYPE machine is entering or consuming the name (`AfterKeyword`, `BeforeName`, `Name`) and while consuming an already-open double- or single-quoted public/system identifier. Unquoted PUBLIC/SYSTEM transitions, malformed recovery states, comments, and bogus comments remain fail closed. Source columns follow the pinned html5lib fixture convention: BMP UTF-8 scalars count as one UTF-16 code unit and non-BMP scalars count as two. This bounded distinction prevents unsupported executions from being converted into parse-error-stream regressions.",
    "document bounded quoted identifier states",
)
text = replace_once(
    text,
    "The full-corpus census snapshot is **5660 pass / 129 fail / 1247 unsupported / 7036 total** before this recovery slice. Production tokenizer changes are checked with the census `no-regression` policy:",
    "The frozen v9 authority remains separate from observed production progress. Immediately before this slice, the Unicode-name admission measures **6799 pass / 122 fail / 115 unsupported / 7036 total**; this quoted-identifier admission is accepted only at the measured **6807 pass / 122 fail / 107 unsupported / 7036 total** distribution. Production tokenizer changes are checked with the census `no-regression` policy:",
    "document exact quoted identifier corpus movement",
)
text = replace_once(
    text,
    "- retained non-ASCII identifier/comment and NUL fail-closed boundaries.",
    "- quoted UTF-8 public/system identifiers, including non-BMP EOF and closed-identifier cases;\n- retained unquoted/malformed non-ASCII transition, comment, bogus-comment, and NUL fail-closed boundaries.",
    "document quoted identifier focused verification",
)
doc.write_text(text, encoding="utf-8")

print("applied bounded quoted Unicode DOCTYPE identifier production patch")
