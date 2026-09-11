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
    """        if (!ascii_byte(value)) {
            if (state == DoctypeState::AfterKeyword ||
                state == DoctypeState::BeforeName ||
                state == DoctypeState::Name ||
                state == DoctypeState::AfterName ||
                state == DoctypeState::AfterPublicKeyword ||
                state == DoctypeState::PublicIdentifierDoubleQuoted ||
                state == DoctypeState::PublicIdentifierSingleQuoted ||
                state == DoctypeState::AfterPublicIdentifier ||
                state == DoctypeState::AfterSystemKeyword ||
                state == DoctypeState::SystemIdentifierDoubleQuoted ||
                state == DoctypeState::SystemIdentifierSingleQuoted ||
                state == DoctypeState::AfterSystemIdentifier ||
                state == DoctypeState::Bogus) {
                return true;
            }
            return fail_unsupported_doctype_ascii_boundary(
                original_missing_whitespace_before_name,
                state);
        }
""",
    "bounded Unicode recovery-state observer admission",
)
source.write_text(text, encoding="utf-8")


tests = ROOT / "tests/html_tokenizer_markup_declarations_v1_tests.cpp"
text = tests.read_text(encoding="utf-8")
new_test = r'''bool test_utf8_doctype_recovery_states() {
    const std::string scalar("\xF0\x90\x80\x80", 4U);

    std::string after_name = "<!DOCTYPE a ";
    after_name += scalar;
    if (!run_doctype_case(
            "non-BMP UTF-8 after DOCTYPE name",
            after_name,
            "a",
            true,
            {ExpectedError{"invalid-character-sequence-after-doctype-name", 1U, 13U}})) {
        return false;
    }

    std::string bogus = "<!DOCTYPE a a";
    bogus += scalar;
    if (!run_doctype_case(
            "non-BMP UTF-8 in bogus DOCTYPE recovery",
            bogus,
            "a",
            true,
            {ExpectedError{"invalid-character-sequence-after-doctype-name", 1U, 13U}})) {
        return false;
    }

    std::string after_public_keyword = "<!DOCTYPE a PUBLIC";
    after_public_keyword += scalar;
    if (!run_doctype_full_case(
            "non-BMP UTF-8 after PUBLIC keyword",
            after_public_keyword,
            "a",
            false,
            {},
            false,
            {},
            true,
            {ExpectedError{"missing-quote-before-doctype-public-identifier", 1U, 19U}})) {
        return false;
    }

    std::string after_public_identifier = "<!DOCTYPE a PUBLIC''";
    after_public_identifier += scalar;
    if (!run_doctype_full_case(
            "non-BMP UTF-8 after PUBLIC identifier",
            after_public_identifier,
            "a",
            true,
            {},
            false,
            {},
            true,
            {
                ExpectedError{"missing-whitespace-after-doctype-public-keyword", 1U, 19U},
                ExpectedError{"missing-quote-before-doctype-system-identifier", 1U, 21U},
            })) {
        return false;
    }

    std::string after_system_keyword = "<!DOCTYPE a SYSTEM";
    after_system_keyword += scalar;
    if (!run_doctype_full_case(
            "non-BMP UTF-8 after SYSTEM keyword",
            after_system_keyword,
            "a",
            false,
            {},
            false,
            {},
            true,
            {ExpectedError{"missing-quote-before-doctype-system-identifier", 1U, 19U}})) {
        return false;
    }

    std::string after_system_identifier = "<!DOCTYPE a SYSTEM''";
    after_system_identifier += scalar;
    if (!run_doctype_full_case(
            "non-BMP UTF-8 after SYSTEM identifier",
            after_system_identifier,
            "a",
            false,
            {},
            true,
            {},
            false,
            {
                ExpectedError{"missing-whitespace-after-doctype-system-keyword", 1U, 19U},
                ExpectedError{"unexpected-character-after-doctype-system-identifier", 1U, 21U},
            })) {
        return false;
    }

    std::string no_space_public = "<!DOCTYPEa PUBLIC";
    no_space_public += scalar;
    return run_doctype_full_case(
        "non-BMP UTF-8 PUBLIC recovery after no-space DOCTYPE name",
        no_space_public,
        "a",
        false,
        {},
        false,
        {},
        true,
        {
            ExpectedError{"missing-whitespace-before-doctype-name", 1U, 10U},
            ExpectedError{"missing-quote-before-doctype-public-identifier", 1U, 18U},
        });
}

'''
text = replace_once(
    text,
    """bool test_bounded_ascii_doctype_recovery() {
""",
    new_test + """bool test_bounded_ascii_doctype_recovery() {
""",
    "insert Unicode recovery-state regressions",
)
text = replace_once(
    text,
    """        !test_utf8_doctype_quoted_identifiers() ||
        !test_bounded_ascii_doctype_recovery() ||
""",
    """        !test_utf8_doctype_quoted_identifiers() ||
        !test_utf8_doctype_recovery_states() ||
        !test_bounded_ascii_doctype_recovery() ||
""",
    "register Unicode recovery-state regressions",
)
tests.write_text(text, encoding="utf-8")


doc = ROOT / "docs/Z7_HTML_TOKENIZER_MARKUP_DECLARATIONS_V1.md"
text = doc.read_text(encoding="utf-8")
text = replace_once(
    text,
    "- invalid sequences after a DOCTYPE name and unexpected characters after a system identifier through the bogus-DOCTYPE state;",
    "- invalid sequences after a DOCTYPE name and unexpected characters after a system identifier through the bogus-DOCTYPE state, including valid UTF-8 recovery bytes;",
    "document UTF-8 recovery states",
)
text = replace_once(
    text,
    "- non-ASCII DOCTYPE PUBLIC/SYSTEM transition and malformed-recovery authority outside quoted identifier payload states;",
    "- non-ASCII DOCTYPE authority in the still-unadmitted `BeforePublicIdentifier`, `BetweenPublicAndSystemIdentifiers`, and `BeforeSystemIdentifier` transition states;",
    "narrow retained DOCTYPE Unicode boundary",
)
text = replace_once(
    text,
    "Non-ASCII bytes are admitted while the DOCTYPE machine is entering or consuming the name (`AfterKeyword`, `BeforeName`, `Name`) and while consuming an already-open double- or single-quoted public/system identifier. Unquoted PUBLIC/SYSTEM transitions, malformed recovery states, comments, and bogus comments remain fail closed. Source columns follow the pinned html5lib fixture convention: BMP UTF-8 scalars count as one UTF-16 code unit and non-BMP scalars count as two. This bounded distinction prevents unsupported executions from being converted into parse-error-stream regressions.",
    "Non-ASCII bytes are admitted while the DOCTYPE machine is entering or consuming the name (`AfterKeyword`, `BeforeName`, `Name`), while consuming an already-open double- or single-quoted public/system identifier, and in the corpus-proven recovery states `AfterName`, `AfterPublicKeyword`, `AfterPublicIdentifier`, `AfterSystemKeyword`, `AfterSystemIdentifier`, and `Bogus`. The still-unproven `BeforePublicIdentifier`, `BetweenPublicAndSystemIdentifiers`, and `BeforeSystemIdentifier` transition states remain fail closed, as do comment and bogus-comment preprocessing gaps. Source columns follow the pinned html5lib fixture convention: BMP UTF-8 scalars count as one UTF-16 code unit and non-BMP scalars count as two. This bounded distinction prevents unsupported executions from being converted into parse-error-stream regressions.",
    "document exact Unicode recovery-state boundary",
)
text = replace_once(
    text,
    "The frozen v9 authority remains separate from observed production progress. Immediately before this slice, the Unicode-name admission measures **6799 pass / 122 fail / 115 unsupported / 7036 total**; this quoted-identifier admission is accepted only at the measured **6807 pass / 122 fail / 107 unsupported / 7036 total** distribution. Production tokenizer changes are checked with the census `no-regression` policy:",
    "The frozen v9 authority remains separate from observed production progress. Immediately before this slice, quoted Unicode identifiers measure **6807 pass / 122 fail / 107 unsupported / 7036 total**; this bounded recovery-state admission is accepted only at the measured **6819 pass / 122 fail / 95 unsupported / 7036 total** distribution. The two remaining non-NUL DOCTYPE unsupported buckets measured by the prior slice (`malformed DOCTYPE-name transition` and `PUBLIC/SYSTEM or malformed DOCTYPE recovery`) both fall from six executions to zero. Production tokenizer changes are checked with the census `no-regression` policy:",
    "document exact recovery-state corpus movement",
)
text = replace_once(
    text,
    "- quoted UTF-8 public/system identifiers, including non-BMP EOF and closed-identifier cases;\n- retained unquoted/malformed non-ASCII transition, comment, bogus-comment, and NUL fail-closed boundaries.",
    "- quoted UTF-8 public/system identifiers, including non-BMP EOF and closed-identifier cases;\n- non-BMP UTF-8 recovery after a name, in bogus DOCTYPE recovery, after PUBLIC/SYSTEM keywords, and after public/system identifiers;\n- retained unproven before-identifier transition, comment, bogus-comment, and NUL fail-closed boundaries.",
    "document recovery-state focused verification",
)
doc.write_text(text, encoding="utf-8")

print("applied bounded Unicode DOCTYPE recovery-state production patch")
