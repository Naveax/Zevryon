#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/html_tokenizer_data_tags_v1.cpp"
TESTS = ROOT / "tests/html_tokenizer_data_tags_v1_tests.cpp"
DOC = ROOT / "docs/Z7_HTML_TOKENIZER_DATA_TAGS_V1.md"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


def patch_source() -> None:
    text = SOURCE.read_text(encoding="utf-8")

    text = replace_once(
        text,
        """bool tag_name_character(char value) noexcept {\n    return ascii_alpha(value) || ascii_digit(value) || value == '-';\n}\n\n""",
        "",
        "remove obsolete tag-name whitelist helper",
    )

    text = replace_once(
        text,
        """    bool parse_attribute(\n        std::size_t* cursor,\n        std::vector<HtmlTokenizerV1Attribute>* attributes,\n        std::size_t* token_bytes,\n        bool missing_whitespace_before) {\n""",
        """    bool parse_attribute(\n        std::size_t* cursor,\n        std::vector<HtmlTokenizerV1Attribute>* attributes,\n        std::size_t* token_bytes,\n        bool missing_whitespace_before,\n        bool leading_control_error_already_emitted) {\n""",
        "extend attribute parser recovery context",
    )

    text = replace_once(
        text,
        """        const std::size_t name_begin = *cursor;\n        HtmlTokenizerV1Attribute attribute;\n        bool leading_control_error_emitted = false;\n""",
        """        const std::size_t name_begin = *cursor;\n        HtmlTokenizerV1Attribute attribute;\n        bool leading_control_error_emitted =\n            leading_control_error_already_emitted;\n""",
        "seed pre-emitted control diagnostic",
    )

    text = replace_once(
        text,
        """        if (missing_whitespace_before &&\n            ascii_byte(input_[*cursor]) &&\n            ascii_control_parse_error(input_[*cursor])) {\n""",
        """        if (missing_whitespace_before &&\n            !leading_control_error_emitted &&\n            ascii_byte(input_[*cursor]) &&\n            ascii_control_parse_error(input_[*cursor])) {\n""",
        "avoid duplicate leading control diagnostic",
    )

    text = replace_once(
        text,
        """        std::string name;\n        while (probe < input_.size() && tag_name_character(input_[probe])) {\n            if (!append_name_character(&name, input_[probe])) {\n                return false;\n            }\n            ++probe;\n        }\n""",
        """        std::string name;\n        while (probe < input_.size()) {\n            const char character = input_[probe];\n            if (ascii_space(character) || character == '/' || character == '>') {\n                break;\n            }\n            if (character == '\\0') {\n                return fail_data_tokenizer(\n                    error_,\n                    \"HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented\");\n            }\n            if (!ascii_byte(character)) {\n                // Preserve the historical census bucket until Unicode tag-name\n                // preprocessing/location authority is admitted.\n                return fail_data_tokenizer(\n                    error_,\n                    \"HTML Data-tag tokenizer first attribute without separating whitespace is outside admitted v1 subset\");\n            }\n            if (ascii_control_parse_error(character) &&\n                !emit_parse_error(probe, \"control-character-in-input-stream\")) {\n                return false;\n            }\n            if (!append_name_character(&name, character)) {\n                return false;\n            }\n            ++probe;\n        }\n""",
        "admit bounded ASCII tag-name state",
    )

    text = replace_once(
        text,
        """        bool self_closing = false;\n        bool parsed_attribute = false;\n\n        while (true) {\n""",
        """        bool self_closing = false;\n        bool parsed_attribute = false;\n        bool solidus_reconsume_allows_attribute = false;\n        bool leading_control_error_emitted = false;\n\n        while (true) {\n""",
        "add self-closing recovery state",
    )

    old_solidus = """            if (input_[probe] == '/' &&\n                probe + 1U < input_.size() &&\n                input_[probe + 1U] == '>') {\n                self_closing = true;\n                const std::size_t close_offset = probe + 1U;\n                probe += 2U;\n                if (end_tag && !attributes.empty()) {\n                    if (!emit_parse_error(\n                            close_offset,\n                            \"end-tag-with-attributes\")) {\n                        return false;\n                    }\n                    attributes.clear();\n                }\n                if (end_tag &&\n                    !emit_parse_error(\n                        close_offset,\n                        \"end-tag-with-trailing-solidus\")) {\n                    return false;\n                }\n                if (!emit_tag(\n                        std::move(name),\n                        std::move(attributes),\n                        end_tag,\n                        self_closing)) {\n                    return false;\n                }\n                *cursor = probe;\n                return true;\n            }\n\n            bool missing_whitespace_before = false;\n            if (!had_whitespace) {\n                if (!parsed_attribute) {\n                    return fail_data_tokenizer(\n                        error_,\n                        \"HTML Data-tag tokenizer first attribute without separating whitespace is outside admitted v1 subset\");\n                }\n                missing_whitespace_before = true;\n            }\n\n            if (!parse_attribute(\n                    &probe,\n                    &attributes,\n                    &token_bytes,\n                    missing_whitespace_before)) {\n                return false;\n            }\n            parsed_attribute = true;\n"""

    new_solidus = """            if (input_[probe] == '/') {\n                if (probe + 1U >= input_.size()) {\n                    // Generic EOF recovery is a separate canonical pass. Keep\n                    // the pre-v5 unsupported classification stable for now.\n                    return fail_data_tokenizer(\n                        error_,\n                        \"HTML Data-tag tokenizer first attribute without separating whitespace is outside admitted v1 subset\");\n                }\n                if (input_[probe + 1U] == '>') {\n                    self_closing = true;\n                    const std::size_t close_offset = probe + 1U;\n                    probe += 2U;\n                    if (end_tag && !attributes.empty()) {\n                        if (!emit_parse_error(\n                                close_offset,\n                                \"end-tag-with-attributes\")) {\n                            return false;\n                        }\n                        attributes.clear();\n                    }\n                    if (end_tag &&\n                        !emit_parse_error(\n                            close_offset,\n                            \"end-tag-with-trailing-solidus\")) {\n                        return false;\n                    }\n                    if (!emit_tag(\n                            std::move(name),\n                            std::move(attributes),\n                            end_tag,\n                            self_closing)) {\n                        return false;\n                    }\n                    *cursor = probe;\n                    return true;\n                }\n\n                const std::size_t reconsume_offset = probe + 1U;\n                const char reconsume_character = input_[reconsume_offset];\n                if (reconsume_character == '\\0') {\n                    return fail_data_tokenizer(\n                        error_,\n                        \"HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented\");\n                }\n                if (!ascii_byte(reconsume_character)) {\n                    // Do not move the existing non-ASCII debt into a new census\n                    // bucket before Unicode tag-name authority is admitted.\n                    return fail_data_tokenizer(\n                        error_,\n                        \"HTML Data-tag tokenizer first attribute without separating whitespace is outside admitted v1 subset\");\n                }\n                leading_control_error_emitted =\n                    ascii_control_parse_error(reconsume_character);\n                if (leading_control_error_emitted &&\n                    !emit_parse_error(\n                        reconsume_offset,\n                        \"control-character-in-input-stream\")) {\n                    return false;\n                }\n                if (!emit_parse_error(\n                        reconsume_offset,\n                        \"unexpected-solidus-in-tag\")) {\n                    return false;\n                }\n                probe = reconsume_offset;\n                solidus_reconsume_allows_attribute = true;\n                continue;\n            }\n\n            bool missing_whitespace_before = false;\n            if (!had_whitespace) {\n                if (!parsed_attribute &&\n                    !solidus_reconsume_allows_attribute) {\n                    return fail_data_tokenizer(\n                        error_,\n                        \"HTML Data-tag tokenizer first attribute without separating whitespace is outside admitted v1 subset\");\n                }\n                missing_whitespace_before =\n                    parsed_attribute && !solidus_reconsume_allows_attribute;\n            }\n\n            if (!parse_attribute(\n                    &probe,\n                    &attributes,\n                    &token_bytes,\n                    missing_whitespace_before,\n                    leading_control_error_emitted)) {\n                return false;\n            }\n            parsed_attribute = true;\n            solidus_reconsume_allows_attribute = false;\n            leading_control_error_emitted = false;\n"""
    text = replace_once(
        text,
        old_solidus,
        new_solidus,
        "admit self-closing start-tag recovery",
    )

    SOURCE.write_text(text, encoding="utf-8", newline="\n")


def patch_tests() -> None:
    text = TESTS.read_text(encoding="utf-8")
    marker = "bool test_admitted_references_and_fail_closed_boundaries() {\n"
    new_test = r'''bool test_tag_name_and_self_closing_state_recovery() {
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("<a<b>", {}, &sink, nullptr, &error),
                std::string("less-than tag-name state: ") + error)) {
            return false;
        }
        if (!require(sink.tokens.size() == 1U, "less-than tag-name token count") ||
            !require(
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::StartTag &&
                    sink.tokens[0].name == "a<b" &&
                    sink.tokens[0].attributes.empty(),
                "less-than byte remains in tag name") ||
            !require(sink.errors.empty(), "less-than tag-name emits no parse error")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("<h/a='b'>", {}, &sink, nullptr, &error),
                std::string("solidus reconsume: ") + error)) {
            return false;
        }
        if (!require(sink.tokens.size() == 1U, "solidus reconsume token count") ||
            !require(sink.tokens[0].name == "h", "solidus reconsume tag name") ||
            !require(!sink.tokens[0].self_closing, "solidus reconsume is not self-closing") ||
            !require(sink.tokens[0].attributes.size() == 1U, "solidus reconsume attribute count") ||
            !require(attribute_is(sink.tokens[0].attributes[0], "a", "b"), "solidus reconsume attribute") ||
            !require(sink.errors.size() == 1U, "solidus reconsume error count") ||
            !require(
                sink.errors[0].code == "unexpected-solidus-in-tag" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == 4U,
                "solidus reconsume error position")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string input = "<a/";
        input.push_back('\x0B');
        input.push_back('>');
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                std::string("solidus control reconsume: ") + error)) {
            return false;
        }
        const std::string control_name(1U, '\x0B');
        if (!require(sink.tokens.size() == 1U, "solidus control token count") ||
            !require(sink.tokens[0].attributes.size() == 1U, "solidus control attribute count") ||
            !require(attribute_is(sink.tokens[0].attributes[0], control_name, ""), "solidus control attribute") ||
            !require(sink.errors.size() == 2U, "solidus control error count") ||
            !require(
                sink.errors[0].code == "control-character-in-input-stream" &&
                    sink.errors[0].column == 4U,
                "solidus control input error is first") ||
            !require(
                sink.errors[1].code == "unexpected-solidus-in-tag" &&
                    sink.errors[1].column == 4U,
                "solidus tokenizer error follows input error")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string input = "<a";
        input.push_back('\x0B');
        input.push_back('>');
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                std::string("tag-name control byte: ") + error)) {
            return false;
        }
        std::string expected_name = "a";
        expected_name.push_back('\x0B');
        if (!require(sink.tokens.size() == 1U, "tag-name control token count") ||
            !require(sink.tokens[0].name == expected_name, "tag-name control byte retained") ||
            !require(sink.errors.size() == 1U, "tag-name control error count") ||
            !require(
                sink.errors[0].code == "control-character-in-input-stream" &&
                    sink.errors[0].column == 3U,
                "tag-name control error position")) {
            return false;
        }
    }
    return true;
}

'''
    text = replace_once(
        text,
        marker,
        new_test + marker,
        "insert tag-name/self-closing focused tests",
    )
    text = replace_once(
        text,
        """        !test_attribute_name_state_recovery() ||\n        !test_end_tag_attributes_are_diagnosed_and_dropped() ||\n""",
        """        !test_attribute_name_state_recovery() ||\n        !test_tag_name_and_self_closing_state_recovery() ||\n        !test_end_tag_attributes_are_diagnosed_and_dropped() ||\n""",
        "register tag-name/self-closing focused tests",
    )
    TESTS.write_text(text, encoding="utf-8", newline="\n")


def patch_doc() -> None:
    text = DOC.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "- ASCII-case-insensitive tag names plus bounded ASCII attribute-name state handling, with ASCII letters normalized to lowercase;\n",
        "- bounded ASCII tag-name and attribute-name state handling, with ASCII letters normalized to lowercase;\n",
        "expand admitted tag-name documentation",
    )
    text = replace_once(
        text,
        """- `end-tag-with-trailing-solidus`, emitting an ordinary EndTag;\n- tag-open invalid ASCII `anything else`: `invalid-first-character-of-tag-name`, literal `<` Character output, then reconsume in Data;\n""",
        """- `end-tag-with-trailing-solidus`, emitting an ordinary EndTag;\n- ASCII tag-name `anything else` bytes are retained in the normalized tag name, while admitted C0/DEL controls emit `control-character-in-input-stream`;\n- self-closing-start-tag recovery: a `/` not followed by `>` emits `unexpected-solidus-in-tag` and reconsumes the following admitted ASCII byte in before-attribute-name, preserving input-stream error ordering;\n- tag-open invalid ASCII `anything else`: `invalid-first-character-of-tag-name`, literal `<` Character output, then reconsume in Data;\n""",
        "document tag-name/self-closing recovery",
    )
    text = replace_once(
        text,
        "- non-ASCII raw-input preprocessing/location authority, including non-ASCII attribute names;\n- remaining malformed tag/attribute recovery not explicitly admitted above;\n",
        "- non-ASCII raw-input preprocessing/location authority, including non-ASCII tag and attribute names;\n- generic EOF-in-tag/self-closing recovery and remaining malformed tag/attribute recovery not explicitly admitted above;\n",
        "clarify remaining fail-closed boundary",
    )
    DOC.write_text(text, encoding="utf-8", newline="\n")


def main() -> None:
    patch_source()
    patch_tests()
    patch_doc()


if __name__ == "__main__":
    main()
