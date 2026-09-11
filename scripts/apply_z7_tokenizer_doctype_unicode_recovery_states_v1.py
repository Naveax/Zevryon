#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/html_tokenizer_markup_declarations_v1.cpp"

text = SOURCE.read_text(encoding="utf-8")
old = """        if (!ascii_byte(value)) {
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
"""
new = """        if (!ascii_byte(value)) {
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
"""
count = text.count(old)
if count != 1:
    raise SystemExit(f"observer anchor: expected one match, found {count}")
SOURCE.write_text(text.replace(old, new, 1), encoding="utf-8")
print("applied bounded Unicode DOCTYPE recovery-state diagnostic patch")
