#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


# Token model: preserve null-vs-empty DOCTYPE names.
path = ROOT / "src/html_tokenizer_token_stream_v1.hpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''    // DOCTYPE-only fields. Presence is explicit because the external tokenizer
    // authority distinguishes a missing identifier (null) from an empty one.
    std::string public_identifier;
''',
    '''    // DOCTYPE-only fields. Presence is explicit because the external tokenizer
    // authority distinguishes a missing name/identifier (null) from an empty one.
    bool has_doctype_name{false};
    std::string public_identifier;
''',
    "DOCTYPE name presence field",
)
path.write_text(text, encoding="utf-8")


# Markup declarations: bounded missing-name recovery and propagation.
path = ROOT / "src/html_tokenizer_markup_declarations_v1.cpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''    bool emit_doctype(
        std::string name,
        std::string public_identifier,
        bool has_public_identifier,
        std::string system_identifier,
        bool has_system_identifier,
        bool force_quirks) {
''',
    '''    bool emit_doctype(
        std::string name,
        std::string public_identifier,
        bool has_public_identifier,
        std::string system_identifier,
        bool has_system_identifier,
        bool force_quirks,
        bool has_doctype_name = true) {
''',
    "emit_doctype signature",
)
text = replace_once(
    text,
    '''        HtmlTokenizerV1Token token;
        token.kind = HtmlTokenizerV1TokenKind::Doctype;
        token.name = std::move(name);
        token.public_identifier = std::move(public_identifier);
''',
    '''        HtmlTokenizerV1Token token;
        token.kind = HtmlTokenizerV1TokenKind::Doctype;
        token.name = std::move(name);
        token.has_doctype_name = has_doctype_name;
        token.public_identifier = std::move(public_identifier);
''',
    "emit_doctype name presence",
)
text = replace_once(
    text,
    '''    bool finish_doctype(
        std::size_t next_offset,
        std::string name,
        std::string public_identifier,
        bool has_public_identifier,
        std::string system_identifier,
        bool has_system_identifier,
        bool force_quirks) {
''',
    '''    bool finish_doctype(
        std::size_t next_offset,
        std::string name,
        std::string public_identifier,
        bool has_public_identifier,
        std::string system_identifier,
        bool has_system_identifier,
        bool force_quirks,
        bool has_doctype_name = true) {
''',
    "finish_doctype signature",
)
text = replace_once(
    text,
    '''                std::move(system_identifier),
                has_system_identifier,
                force_quirks)) {
''',
    '''                std::move(system_identifier),
                has_system_identifier,
                force_quirks,
                has_doctype_name)) {
''',
    "finish_doctype forwards name presence",
)
text = replace_once(
    text,
    '''            if (cursor >= input_.size()) {
                if (state == DoctypeState::AfterKeyword) {
                    return fail_markup(
                        error_,
                        "HTML markup declaration malformed DOCTYPE-name transition is outside admitted v1 recovery");
                }
                if (state == DoctypeState::BeforeName) {
                    return fail_markup(
                        error_,
                        "HTML markup declaration missing DOCTYPE name recovery is outside admitted v1 subset");
                }
                if (state != DoctypeState::Bogus) {
''',
    '''            if (cursor >= input_.size()) {
                if (state == DoctypeState::AfterKeyword ||
                    state == DoctypeState::BeforeName) {
                    if (!emit_parse_error(input_.size(), "eof-in-doctype")) {
                        return false;
                    }
                    force_quirks = true;
                    return finish_doctype(
                        input_.size(),
                        std::move(name),
                        std::move(public_identifier),
                        has_public_identifier,
                        std::move(system_identifier),
                        has_system_identifier,
                        force_quirks,
                        false);
                }
                if (state != DoctypeState::Bogus) {
''',
    "missing DOCTYPE name EOF recovery",
)
text = replace_once(
    text,
    '''                if (value == '>') {
                    return fail_markup(
                        error_,
                        "HTML markup declaration malformed DOCTYPE-name transition is outside admitted v1 recovery");
                }
''',
    '''                if (value == '>') {
                    if (!emit_parse_error(cursor, "missing-doctype-name")) {
                        return false;
                    }
                    force_quirks = true;
                    return finish_doctype(
                        cursor + 1U,
                        std::move(name),
                        std::move(public_identifier),
                        has_public_identifier,
                        std::move(system_identifier),
                        has_system_identifier,
                        force_quirks,
                        false);
                }
''',
    "AfterKeyword missing-name close recovery",
)
text = replace_once(
    text,
    '''                if (value == '>') {
                    return fail_markup(
                        error_,
                        "HTML markup declaration missing DOCTYPE name recovery is outside admitted v1 subset");
                }
''',
    '''                if (value == '>') {
                    if (!emit_parse_error(cursor, "missing-doctype-name")) {
                        return false;
                    }
                    force_quirks = true;
                    return finish_doctype(
                        cursor + 1U,
                        std::move(name),
                        std::move(public_identifier),
                        has_public_identifier,
                        std::move(system_identifier),
                        has_system_identifier,
                        force_quirks,
                        false);
                }
''',
    "BeforeName missing-name close recovery",
)
path.write_text(text, encoding="utf-8")


# Focused markup-declaration regressions.
path = ROOT / "tests/html_tokenizer_markup_declarations_v1_tests.cpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''bool run_doctype_full_case(
    std::string_view label,
    std::string_view input,
    std::string_view expected_name,
    bool expected_has_public_identifier,
    std::string_view expected_public_identifier,
    bool expected_has_system_identifier,
    std::string_view expected_system_identifier,
    bool expected_force_quirks,
    const std::vector<ExpectedError>& expected_errors = {}) {
''',
    '''bool run_doctype_full_case(
    std::string_view label,
    std::string_view input,
    std::string_view expected_name,
    bool expected_has_public_identifier,
    std::string_view expected_public_identifier,
    bool expected_has_system_identifier,
    std::string_view expected_system_identifier,
    bool expected_force_quirks,
    const std::vector<ExpectedError>& expected_errors = {},
    bool expected_has_doctype_name = true) {
''',
    "focused helper name-presence argument",
)
text = replace_once(
    text,
    '''    const HtmlTokenizerV1Token& token = sink.tokens[0];
    return require(token.kind == HtmlTokenizerV1TokenKind::Doctype, std::string(label) + " kind") &&
        require(token.name == expected_name, std::string(label) + " normalized name") &&
''',
    '''    const HtmlTokenizerV1Token& token = sink.tokens[0];
    return require(token.kind == HtmlTokenizerV1TokenKind::Doctype, std::string(label) + " kind") &&
        require(token.has_doctype_name == expected_has_doctype_name,
                std::string(label) + " name presence") &&
        require(token.name == expected_name, std::string(label) + " normalized name") &&
''',
    "focused helper name-presence assertion",
)
missing_name_tests = r'''bool test_missing_doctype_name_recovery() {
    return run_doctype_full_case(
               "missing name immediate close",
               "<!DOCTYPE>",
               "",
               false,
               {},
               false,
               {},
               true,
               {ExpectedError{"missing-doctype-name", 1U, 10U}},
               false) &&
        run_doctype_full_case(
            "missing name after whitespace",
            "<!DOCTYPE >",
            "",
            false,
            {},
            false,
            {},
            true,
            {ExpectedError{"missing-doctype-name", 1U, 11U}},
            false) &&
        run_doctype_full_case(
            "missing name at EOF after keyword",
            "<!DOCTYPE",
            "",
            false,
            {},
            false,
            {},
            true,
            {ExpectedError{"eof-in-doctype", 1U, 10U}},
            false) &&
        run_doctype_full_case(
            "missing name at EOF after whitespace",
            "<!DOCTYPE ",
            "",
            false,
            {},
            false,
            {},
            true,
            {ExpectedError{"eof-in-doctype", 1U, 11U}},
            false);
}

'''
text = replace_once(
    text,
    '''bool test_bounded_ascii_doctype_recovery() {
''',
    missing_name_tests + '''bool test_bounded_ascii_doctype_recovery() {
''',
    "insert missing-name focused tests",
)
# Retire the stale fail-closed regression structurally.
needle = '"missing DOCTYPE name remains fail closed"'
position = text.find(needle)
if position < 0:
    raise SystemExit("stale missing-name regression marker not found")
start = text.rfind("\n    {\n", 0, position)
if start < 0:
    raise SystemExit("stale missing-name regression start not found")
start += 1
end = text.find("\n    {\n", position)
if end < 0:
    raise SystemExit("stale missing-name regression next top-level block not found")
end += 1
text = text[:start] + text[end:]
text = replace_once(
    text,
    '''    if (!test_pinned_test1_doctypes() ||
        !test_bounded_ascii_doctype_recovery() ||
''',
    '''    if (!test_pinned_test1_doctypes() ||
        !test_missing_doctype_name_recovery() ||
        !test_bounded_ascii_doctype_recovery() ||
''',
    "register missing-name focused tests",
)
path.write_text(text, encoding="utf-8")


# Probe wire: explicit DOCTYPE name presence.
path = ROOT / "tests/html_tokenizer_token_stream_v1_probe.cpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''    case HtmlTokenizerV1TokenKind::Doctype:
        std::cout << "TOKEN\\tD\\t" << encode_hex(token.name) << '\\t'
                  << (token.has_public_identifier ? 1 : 0) << '\\t'
''',
    '''    case HtmlTokenizerV1TokenKind::Doctype:
        std::cout << "TOKEN\\tD\\t" << (token.has_doctype_name ? 1 : 0) << '\\t'
                  << encode_hex(token.name) << '\\t'
                  << (token.has_public_identifier ? 1 : 0) << '\\t'
''',
    "probe nullable DOCTYPE name wire",
)
path.write_text(text, encoding="utf-8")


# Full-corpus adapter: consume nullable-name wire and run those executions.
path = ROOT / "scripts/z7_html5lib_tokenizer_full_corpus_census_v1.py"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''            if kind == "D":
                require(len(fields) == 8, f"probe DOCTYPE line {line_number} malformed")
                name = decode_hex(fields[2], "probe DOCTYPE name")
                has_public = parse_bool(fields[3], "probe DOCTYPE public flag")
                public_id = decode_hex(fields[4], "probe DOCTYPE public id")
                has_system = parse_bool(fields[5], "probe DOCTYPE system flag")
                system_id = decode_hex(fields[6], "probe DOCTYPE system id")
                force_quirks = parse_bool(fields[7], "probe DOCTYPE force-quirks")
                require(has_public or public_id == "", "probe absent public id has payload")
                require(has_system or system_id == "", "probe absent system id has payload")
                tokens.append(("D", name, public_id if has_public else None, system_id if has_system else None, force_quirks))
                continue
''',
    '''            if kind == "D":
                require(len(fields) == 9, f"probe DOCTYPE line {line_number} malformed")
                has_name = parse_bool(fields[2], "probe DOCTYPE name flag")
                name = decode_hex(fields[3], "probe DOCTYPE name")
                has_public = parse_bool(fields[4], "probe DOCTYPE public flag")
                public_id = decode_hex(fields[5], "probe DOCTYPE public id")
                has_system = parse_bool(fields[6], "probe DOCTYPE system flag")
                system_id = decode_hex(fields[7], "probe DOCTYPE system id")
                force_quirks = parse_bool(fields[8], "probe DOCTYPE force-quirks")
                require(has_name or name == "", "probe absent DOCTYPE name has payload")
                require(has_public or public_id == "", "probe absent public id has payload")
                require(has_system or system_id == "", "probe absent system id has payload")
                tokens.append(("D", name if has_name else None, public_id if has_public else None, system_id if has_system else None, force_quirks))
                continue
''',
    "full-corpus nullable DOCTYPE probe parser",
)
text = replace_once(
    text,
    '''def has_nullable_doctype_name(tokens: list[tuple[Any, ...]]) -> bool:
    return any(token[0] == "D" and token[1] is None for token in tokens)


''',
    '''''',
    "remove nullable DOCTYPE preclassifier helper",
)
text = replace_once(
    text,
    '''    if state_name not in STATE_MAP:
        return f"initial-state:{state_name}"
    if has_nullable_doctype_name(tokens):
        return "probe-wire-null-doctype-name"
    try:
''',
    '''    if state_name not in STATE_MAP:
        return f"initial-state:{state_name}"
    try:
''',
    "remove nullable DOCTYPE preclassification",
)
path.write_text(text, encoding="utf-8")


# test1 admitted runner shares the probe protocol.
path = ROOT / "scripts/z7_html5lib_tokenizer_test1_admitted_runner_v1.py"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''            if kind == "D":
                require(len(fields) == 8, f"probe DOCTYPE line {line_number} malformed")
                name = decode_hex(fields[2], f"probe DOCTYPE name line {line_number}")
                has_public = parse_bool_field(fields[3], f"probe DOCTYPE public flag line {line_number}")
                public_payload = decode_hex(fields[4], f"probe DOCTYPE public id line {line_number}")
                has_system = parse_bool_field(fields[5], f"probe DOCTYPE system flag line {line_number}")
                system_payload = decode_hex(fields[6], f"probe DOCTYPE system id line {line_number}")
                force_quirks = parse_bool_field(fields[7], f"probe DOCTYPE force-quirks line {line_number}")
                require(has_public or public_payload == "", f"probe DOCTYPE absent public id has payload")
                require(has_system or system_payload == "", f"probe DOCTYPE absent system id has payload")
                tokens.append(
                    (
                        "D",
                        name,
                        public_payload if has_public else None,
                        system_payload if has_system else None,
                        force_quirks,
                    )
                )
                continue
''',
    '''            if kind == "D":
                require(len(fields) == 9, f"probe DOCTYPE line {line_number} malformed")
                has_name = parse_bool_field(fields[2], f"probe DOCTYPE name flag line {line_number}")
                name_payload = decode_hex(fields[3], f"probe DOCTYPE name line {line_number}")
                has_public = parse_bool_field(fields[4], f"probe DOCTYPE public flag line {line_number}")
                public_payload = decode_hex(fields[5], f"probe DOCTYPE public id line {line_number}")
                has_system = parse_bool_field(fields[6], f"probe DOCTYPE system flag line {line_number}")
                system_payload = decode_hex(fields[7], f"probe DOCTYPE system id line {line_number}")
                force_quirks = parse_bool_field(fields[8], f"probe DOCTYPE force-quirks line {line_number}")
                require(has_name or name_payload == "", f"probe DOCTYPE absent name has payload")
                require(has_public or public_payload == "", f"probe DOCTYPE absent public id has payload")
                require(has_system or system_payload == "", f"probe DOCTYPE absent system id has payload")
                tokens.append(
                    (
                        "D",
                        name_payload if has_name else None,
                        public_payload if has_public else None,
                        system_payload if has_system else None,
                        force_quirks,
                    )
                )
                continue
''',
    "test1 nullable DOCTYPE probe parser",
)
text = replace_once(
    text,
    '''            "TOKEN\\tD\\t68746d6c\\t0\\t\\t0\\t\\t1",
''',
    '''            "TOKEN\\tD\\t1\\t68746d6c\\t0\\t\\t0\\t\\t1",
''',
    "test1 probe self-test DOCTYPE wire",
)
path.write_text(text, encoding="utf-8")


# Documentation: remove stale nullable-name boundary and describe admitted recovery.
path = ROOT / "docs/Z7_HTML_TOKENIZER_MARKUP_DECLARATIONS_V1.md"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''- missing-whitespace-before-name recovery for an otherwise representable ASCII name;
''',
    '''- missing-whitespace-before-name recovery for an otherwise representable ASCII name;
- nullable DOCTYPE-name representation plus `missing-doctype-name` / `eof-in-doctype` recovery with force-quirks;
''',
    "markup docs admitted missing-name recovery",
)
text = replace_once(
    text,
    '''- missing DOCTYPE-name tokens whose external wire representation currently requires nullable-name support;
''',
    '''''',
    "markup docs remove nullable-name boundary",
)
text = replace_once(
    text,
    '''- bounded DOCTYPE payload rejection;
- retained non-ASCII and NUL fail-closed boundaries.
''',
    '''- bounded DOCTYPE payload rejection;
- nullable missing-name recovery at `>` and EOF;
- retained non-ASCII and NUL fail-closed boundaries.
''',
    "markup docs focused verification",
)
path.write_text(text, encoding="utf-8")

path = ROOT / "docs/Z7_HTML_TOKENIZER_TOKEN_STREAM_V1.md"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''Raw NUL is state-local rather than globally admitted. PLAINTEXT, RCDATA and RAWTEXT emit `unexpected-null-character` and append U+FFFD for U+0000. CDATA retains its separately admitted literal-NUL authority. Data and Script-data NUL behavior remain outside this slice and stay behind the corpus NUL authority boundary.
''',
    '''Raw NUL is state-local rather than globally admitted. PLAINTEXT, RCDATA, RAWTEXT and Script-data emit `unexpected-null-character` and append U+FFFD for U+0000 in their admitted Character-consuming states. CDATA retains its separately admitted literal-NUL authority. Data/markup/comment/DOCTYPE NUL behavior remains behind the corpus NUL authority boundary.
''',
    "token-stream docs current NUL authority",
)
text = replace_once(
    text,
    '''- fail-closed NUL/input-preprocessing debt;
''',
    '''- state-local NUL recovery plus remaining Data/markup preprocessing debt;
''',
    "token-stream docs focused NUL regression",
)
text = replace_once(
    text,
    '''Still outstanding are explicitly unsupported corpus surfaces such as Data/Script-data NUL handling, broader non-ASCII preprocessing/location authority, nullable DOCTYPE-name probe-wire representation and remaining malformed Data-tag/DOCTYPE/comment recovery buckets.
''',
    '''Still outstanding are explicitly unsupported corpus surfaces such as Data/markup/comment/DOCTYPE NUL handling, broader non-ASCII preprocessing/location authority and remaining malformed Data-tag/DOCTYPE/comment recovery buckets. Nullable DOCTYPE-name probe-wire representation is admitted by this slice.
''',
    "token-stream docs admission boundary",
)
path.write_text(text, encoding="utf-8")

print("applied nullable DOCTYPE name production patch")
