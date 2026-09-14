#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
path = ROOT / "src/html_tokenizer_data_tags_v1.cpp"
text = path.read_text(encoding="utf-8")
old = '''                if (character == '&') {
                    if (!consume_character_reference(
                            cursor,
                            HtmlTokenizerCharacterReferenceV1Context::Attribute,
                            value,
                            "HTML Data-tag tokenizer attribute value")) {
                        return false;
                    }
                    continue;
                }
                if (!append_value_character(value, character)) {
'''
new = '''                if (ascii_control_parse_error(character) &&
                    !emit_parse_error(*cursor, "control-character-in-input-stream")) {
                    return false;
                }
                if (character == '&') {
                    if (!consume_character_reference(
                            cursor,
                            HtmlTokenizerCharacterReferenceV1Context::Attribute,
                            value,
                            "HTML Data-tag tokenizer attribute value")) {
                        return false;
                    }
                    continue;
                }
                if (!append_value_character(value, character)) {
'''
if text.count(old) != 1:
    raise SystemExit(f"quoted attribute-value control anchor count: {text.count(old)}")
text = text.replace(old, new, 1)
old = '''            if (character == '&') {
                if (!consume_character_reference(
                        cursor,
                        HtmlTokenizerCharacterReferenceV1Context::Attribute,
                        value,
                        "HTML Data-tag tokenizer attribute value")) {
                    return false;
                }
                continue;
            }
            if (character == '"' || character == '\\'' || character == '<' ||
'''
new = '''            if (ascii_control_parse_error(character) &&
                !emit_parse_error(*cursor, "control-character-in-input-stream")) {
                return false;
            }
            if (character == '&') {
                if (!consume_character_reference(
                        cursor,
                        HtmlTokenizerCharacterReferenceV1Context::Attribute,
                        value,
                        "HTML Data-tag tokenizer attribute value")) {
                    return false;
                }
                continue;
            }
            if (character == '"' || character == '\\'' || character == '<' ||
'''
if text.count(old) != 1:
    raise SystemExit(f"unquoted attribute-value control anchor count: {text.count(old)}")
text = text.replace(old, new, 1)
path.write_text(text, encoding="utf-8")
print("applied attribute-value input-control diagnostic candidate")
