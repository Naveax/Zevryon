#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


def replace_in_case(text: str, start_marker: str, end_marker: str, old: str, new: str, label: str) -> str:
    start = text.index(start_marker)
    end = text.index(end_marker, start + len(start_marker))
    segment = text[start:end]
    count = segment.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one case-local anchor, found {count}")
    segment = segment.replace(old, new, 1)
    return text[:start] + segment + text[end:]


path = ROOT / "src/html_tokenizer_markup_declarations_v1.cpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''constexpr std::size_t kMaximumConfiguredTokenBytes = 1024U * 1024U;
''',
    '''constexpr std::size_t kMaximumConfiguredTokenBytes = 1024U * 1024U;
constexpr std::string_view kReplacementCharacterUtf8 = "\\xEF\\xBF\\xBD";
''',
    "replacement scalar constant",
)
text = replace_once(
    text,
    '''    bool fail_unsupported_doctype_ascii_boundary(
''',
    '''    bool append_doctype_replacement(
        std::string* destination,
        std::string_view label) {
        if (destination->size() > config_.maximum_token_bytes ||
            kReplacementCharacterUtf8.size() >
                config_.maximum_token_bytes - destination->size()) {
            return fail_markup(
                error_,
                "HTML markup declaration DOCTYPE " + std::string(label) +
                    " exceeds bounded byte limit");
        }
        destination->append(
            kReplacementCharacterUtf8.data(),
            kReplacementCharacterUtf8.size());
        return true;
    }

    bool fail_unsupported_doctype_ascii_boundary(
''',
    "DOCTYPE replacement helper",
)
text = replace_once(
    text,
    '''        if (value == '\\0') {
            return fail_markup(
                error_,
                "HTML markup declaration input preprocessing/NUL replacement is not implemented");
        }
''',
    '''        if (value == '\\0') {
            // NUL ordering is state-specific. Payload states replace it here in
            // the switch; recovery states first emit their state error and then
            // reconsume the same byte in Bogus so unexpected-null follows it.
            return true;
        }
''',
    "DOCTYPE observer NUL gate",
)

# Payload-bearing states emit unexpected-null immediately and append U+FFFD.
text = replace_in_case(
    text,
    '            case DoctypeState::BeforeName:\n',
    '            case DoctypeState::Name:\n',
    '''                if (ascii_space(value)) {
''',
    '''                if (value == '\\0') {
                    if (!emit_parse_error(cursor, "unexpected-null-character") ||
                        !append_doctype_replacement(&name, "name")) {
                        return false;
                    }
                    state = DoctypeState::Name;
                    ++cursor;
                    break;
                }
                if (ascii_space(value)) {
''',
    "BeforeName NUL",
)
text = replace_in_case(
    text,
    '            case DoctypeState::Name:\n',
    '            case DoctypeState::AfterName:\n',
    '''                if (ascii_space(value)) {
''',
    '''                if (value == '\\0') {
                    if (!emit_parse_error(cursor, "unexpected-null-character") ||
                        !append_doctype_replacement(&name, "name")) {
                        return false;
                    }
                    ++cursor;
                    break;
                }
                if (ascii_space(value)) {
''',
    "Name NUL",
)
text = replace_in_case(
    text,
    '            case DoctypeState::PublicIdentifierDoubleQuoted:\n',
    '            case DoctypeState::AfterPublicIdentifier:\n',
    '''                const char quote = state == DoctypeState::PublicIdentifierDoubleQuoted ? '"' : '\\'';
                if (value == quote) {
''',
    '''                const char quote = state == DoctypeState::PublicIdentifierDoubleQuoted ? '"' : '\\'';
                if (value == '\\0') {
                    if (!emit_parse_error(cursor, "unexpected-null-character") ||
                        !append_doctype_replacement(&public_identifier, "public identifier")) {
                        return false;
                    }
                    ++cursor;
                    break;
                }
                if (value == quote) {
''',
    "PUBLIC identifier NUL",
)
text = replace_in_case(
    text,
    '            case DoctypeState::SystemIdentifierDoubleQuoted:\n',
    '            case DoctypeState::AfterSystemIdentifier:\n',
    '''                const char quote = state == DoctypeState::SystemIdentifierDoubleQuoted ? '"' : '\\'';
                if (value == quote) {
''',
    '''                const char quote = state == DoctypeState::SystemIdentifierDoubleQuoted ? '"' : '\\'';
                if (value == '\\0') {
                    if (!emit_parse_error(cursor, "unexpected-null-character") ||
                        !append_doctype_replacement(&system_identifier, "system identifier")) {
                        return false;
                    }
                    ++cursor;
                    break;
                }
                if (value == quote) {
''',
    "SYSTEM identifier NUL",
)

# Recovery states preserve WHATWG error ordering: state error first, then the
# same NUL is reconsumed in Bogus, which emits unexpected-null and ignores it.
for start_marker, end_marker, label in [
    ('            case DoctypeState::AfterName:\n', '            case DoctypeState::AfterPublicKeyword:\n', 'AfterName'),
    ('            case DoctypeState::AfterPublicKeyword:\n', '            case DoctypeState::BeforePublicIdentifier:\n', 'AfterPublicKeyword'),
    ('            case DoctypeState::BeforePublicIdentifier:\n', '            case DoctypeState::PublicIdentifierDoubleQuoted:\n', 'BeforePublicIdentifier'),
    ('            case DoctypeState::AfterPublicIdentifier:\n', '            case DoctypeState::BetweenPublicAndSystemIdentifiers:\n', 'AfterPublicIdentifier'),
    ('            case DoctypeState::BetweenPublicAndSystemIdentifiers:\n', '            case DoctypeState::AfterSystemKeyword:\n', 'BetweenPublicAndSystemIdentifiers'),
    ('            case DoctypeState::AfterSystemKeyword:\n', '            case DoctypeState::BeforeSystemIdentifier:\n', 'AfterSystemKeyword'),
    ('            case DoctypeState::BeforeSystemIdentifier:\n', '            case DoctypeState::SystemIdentifierDoubleQuoted:\n', 'BeforeSystemIdentifier'),
]:
    text = replace_in_case(
        text,
        start_marker,
        end_marker,
        '''                force_quirks = true;
                state = DoctypeState::Bogus;
                ++cursor;
                break;
''',
        '''                force_quirks = true;
                state = DoctypeState::Bogus;
                if (value != '\\0') {
                    ++cursor;
                }
                break;
''',
        f"{label} NUL reconsume",
    )
text = replace_in_case(
    text,
    '            case DoctypeState::AfterSystemIdentifier:\n',
    '            case DoctypeState::Bogus:\n',
    '''                state = DoctypeState::Bogus;
                ++cursor;
                break;
''',
    '''                state = DoctypeState::Bogus;
                if (value != '\\0') {
                    ++cursor;
                }
                break;
''',
    "AfterSystemIdentifier NUL reconsume",
)
text = replace_once(
    text,
    '''            case DoctypeState::Bogus:
                if (value == '>') {
''',
    '''            case DoctypeState::Bogus:
                if (value == '\\0') {
                    if (!emit_parse_error(cursor, "unexpected-null-character")) {
                        return false;
                    }
                    ++cursor;
                    break;
                }
                if (value == '>') {
''',
    "Bogus DOCTYPE NUL",
)
path.write_text(text, encoding="utf-8")


# Representative focused regressions freeze payload replacement and recovery
# error ordering; the full corpus diagnostic covers all pinned variants.
path = ROOT / "tests/html_tokenizer_markup_declarations_v1_tests.cpp"
text = path.read_text(encoding="utf-8")
insert_before = '''bool test_bounded_ascii_doctype_recovery() {
'''
new_test = r'''bool test_doctype_nul_semantics() {
    const std::string replacement("\xEF\xBF\xBD", 3U);
    {
        std::string input = "<!DOCTYPE";
        input.push_back('\0');
        if (!run_doctype_case(
                "NUL reconsumed into DOCTYPE name",
                input,
                replacement,
                true,
                {
                    ExpectedError{"missing-whitespace-before-doctype-name", 1U, 10U},
                    ExpectedError{"unexpected-null-character", 1U, 10U},
                    ExpectedError{"eof-in-doctype", 1U, 11U},
                })) {
            return false;
        }
    }
    {
        std::string input = "<!DOCTYPE a PUBLIC\"";
        input.push_back('\0');
        if (!run_doctype_full_case(
                "NUL in quoted PUBLIC identifier",
                input,
                "a",
                true,
                replacement,
                false,
                {},
                true,
                {
                    ExpectedError{"missing-whitespace-after-doctype-public-keyword", 1U, 19U},
                    ExpectedError{"unexpected-null-character", 1U, 20U},
                    ExpectedError{"eof-in-doctype", 1U, 21U},
                })) {
            return false;
        }
    }
    {
        std::string input = "<!DOCTYPE a ";
        input.push_back('\0');
        if (!run_doctype_case(
                "NUL after DOCTYPE name recovery",
                input,
                "a",
                true,
                {
                    ExpectedError{"invalid-character-sequence-after-doctype-name", 1U, 13U},
                    ExpectedError{"unexpected-null-character", 1U, 13U},
                })) {
            return false;
        }
    }
    {
        std::string input = "<!DOCTYPE a SYSTEM''";
        input.push_back('\0');
        if (!run_doctype_full_case(
                "NUL after SYSTEM identifier recovery",
                input,
                "a",
                false,
                {},
                true,
                {},
                false,
                {
                    ExpectedError{"missing-whitespace-after-doctype-system-keyword", 1U, 19U},
                    ExpectedError{"unexpected-character-after-doctype-system-identifier", 1U, 21U},
                    ExpectedError{"unexpected-null-character", 1U, 21U},
                })) {
            return false;
        }
    }
    return true;
}

'''
text = replace_once(text, insert_before, new_test + insert_before, "DOCTYPE NUL focused tests")
text = replace_once(
    text,
    '''        !test_utf8_doctype_recovery_states() ||
        !test_bounded_ascii_doctype_recovery() ||
''',
    '''        !test_utf8_doctype_recovery_states() ||
        !test_doctype_nul_semantics() ||
        !test_bounded_ascii_doctype_recovery() ||
''',
    "DOCTYPE NUL test registration",
)
path.write_text(text, encoding="utf-8")


path = ROOT / "scripts/z7_html5lib_tokenizer_full_corpus_census_v1.py"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''    # CDATA preserves raw NUL. PLAINTEXT/RCDATA/RAWTEXT and Script data
    # admit their state-specific behavior. Data admits only ordinary character
    # data here; any '<' keeps markup/comment/tag/DOCTYPE NUL fail closed.
    if "\\x00" in input_text:
        if state_name in {
            "PLAINTEXT state",
            "RCDATA state",
            "RAWTEXT state",
            "Script data state",
            "CDATA section state",
        }:
            pass
        elif state_name == "Data state" and "<" not in input_text:
            pass
        else:
            return "input-preprocessing-nul"
''',
    '''    # Diagnostic admission: ordinary Data NUL plus the complete pinned
    # DOCTYPE-NUL family. Comment/tag NUL remains fail closed.
    if "\\x00" in input_text:
        if state_name in {
            "PLAINTEXT state",
            "RCDATA state",
            "RAWTEXT state",
            "Script data state",
            "CDATA section state",
        }:
            pass
        elif state_name == "Data state" and (
            "<" not in input_text or input_text[:9].lower() == "<!doctype"
        ):
            pass
        else:
            return "input-preprocessing-nul"
''',
    "DOCTYPE NUL diagnostic classifier",
)
path.write_text(text, encoding="utf-8")

print("applied DOCTYPE NUL semantics diagnostic patch")
