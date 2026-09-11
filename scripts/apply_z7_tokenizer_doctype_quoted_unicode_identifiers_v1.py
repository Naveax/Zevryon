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
    """                if (state != DoctypeState::Bogus) {
                    if (!emit_parse_error(input_.size(), "eof-in-doctype")) {
                        return false;
                    }
                    force_quirks = true;
                }
""",
    """                if (state != DoctypeState::Bogus) {
                    if (!emit_parse_error(input_.size(), "eof-in-doctype")) {
                        return false;
                    }
                    if (state != DoctypeState::PublicIdentifierDoubleQuoted &&
                        state != DoctypeState::PublicIdentifierSingleQuoted &&
                        state != DoctypeState::SystemIdentifierDoubleQuoted &&
                        state != DoctypeState::SystemIdentifierSingleQuoted) {
                        force_quirks = true;
                    }
                }
""",
    "quoted DOCTYPE identifier EOF force-quirks",
)
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
            false,
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
    "retain unquoted non-ASCII DOCTYPE identifier fail-closed regression",
)
tests.write_text(text, encoding="utf-8")

print("applied quoted Unicode DOCTYPE identifier + EOF quirks diagnostic patch")
