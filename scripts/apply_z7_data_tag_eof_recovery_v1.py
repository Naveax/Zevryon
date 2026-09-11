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
        """    bool append_name_character(std::string* value, char character) {\n""",
        """    bool recover_eof_before_tag_name(\n        std::size_t* cursor,\n        std::string_view literal) {\n        if (!emit_parse_error(input_.size(), \"eof-before-tag-name\")) {\n            return false;\n        }\n        for (const char character : literal) {\n            if (!append_character(character)) {\n                return false;\n            }\n        }\n        *cursor = input_.size();\n        return true;\n    }\n\n    bool recover_eof_in_tag(std::size_t* cursor) {\n        if (!emit_parse_error(input_.size(), \"eof-in-tag\")) {\n            return false;\n        }\n        *cursor = input_.size();\n        return true;\n    }\n\n    bool append_name_character(std::string* value, char character) {\n""",
        "insert bounded EOF recovery helpers",
    )

    text = replace_once(
        text,
        """    bool parse_attribute_value(\n        std::size_t* cursor,\n        std::string* value) {\n        if (*cursor >= input_.size()) {\n            return fail_data_tokenizer(\n                error_,\n                \"HTML Data-tag tokenizer EOF after attribute equals is outside admitted recovery\");\n        }\n""",
        """    bool parse_attribute_value(\n        std::size_t* cursor,\n        std::string* value) {\n        if (*cursor >= input_.size()) {\n            // The outer tag state owns the single canonical eof-in-tag error.\n            return true;\n        }\n""",
        "admit EOF before attribute value",
    )

    text = replace_once(
        text,
        """            if (*cursor == input_.size()) {\n                return fail_data_tokenizer(\n                    error_,\n                    \"HTML Data-tag tokenizer EOF in quoted attribute value is outside admitted recovery\");\n            }\n""",
        """            if (*cursor == input_.size()) {\n                // Preserve the unterminated value only as transient state; the\n                // incomplete tag is discarded when the outer state emits EOF.\n                return true;\n            }\n""",
        "admit EOF in quoted attribute value",
    )

    text = replace_once(
        text,
        """        std::size_t probe = tag_open + 1U;\n        if (probe >= input_.size()) {\n            return fail_data_tokenizer(\n                error_,\n                \"HTML Data-tag tokenizer EOF after tag-open is outside admitted recovery\");\n        }\n""",
        """        std::size_t probe = tag_open + 1U;\n        if (probe >= input_.size()) {\n            return recover_eof_before_tag_name(cursor, \"<\");\n        }\n""",
        "admit EOF after tag-open",
    )

    text = replace_once(
        text,
        """            ++probe;\n            if (probe >= input_.size()) {\n                return fail_data_tokenizer(\n                    error_,\n                    \"HTML Data-tag tokenizer EOF after end-tag open is outside admitted recovery\");\n            }\n""",
        """            ++probe;\n            if (probe >= input_.size()) {\n                return recover_eof_before_tag_name(cursor, \"</\");\n            }\n""",
        "admit EOF after end-tag-open",
    )

    text = replace_once(
        text,
        """            if (probe >= input_.size()) {\n                return fail_data_tokenizer(\n                    error_,\n                    \"HTML Data-tag tokenizer EOF in tag is outside admitted recovery\");\n            }\n""",
        """            if (probe >= input_.size()) {\n                // WHATWG EOF in tag/attribute states emits eof-in-tag and\n                // discards the current incomplete tag token.\n                return recover_eof_in_tag(cursor);\n            }\n""",
        "admit EOF in tag family",
    )

    text = replace_once(
        text,
        """            if (input_[probe] == '/') {\n                if (probe + 1U >= input_.size()) {\n                    // Generic EOF recovery is a separate canonical pass. Keep\n                    // the pre-v5 unsupported classification stable for now.\n                    return fail_data_tokenizer(\n                        error_,\n                        \"HTML Data-tag tokenizer first attribute without separating whitespace is outside admitted v1 subset\");\n                }\n""",
        """            if (input_[probe] == '/') {\n                if (probe + 1U >= input_.size()) {\n                    return recover_eof_in_tag(cursor);\n                }\n""",
        "admit EOF in self-closing-start-tag state",
    )

    SOURCE.write_text(text, encoding="utf-8", newline="\n")


def patch_tests() -> None:
    text = TESTS.read_text(encoding="utf-8")
    marker = "bool test_admitted_references_and_fail_closed_boundaries() {\n"
    new_test = r'''bool test_tag_eof_recovery() {
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("<", {}, &sink, nullptr, &error),
                std::string("EOF after tag-open: ") + error) ||
            !require(sink.tokens.size() == 1U, "tag-open EOF character token count") ||
            !require(
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                    sink.tokens[0].data == "<",
                "tag-open EOF literal recovery") ||
            !require(sink.errors.size() == 1U, "tag-open EOF error count") ||
            !require(
                sink.errors[0].code == "eof-before-tag-name" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == 2U,
                "tag-open EOF error position")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("</", {}, &sink, nullptr, &error),
                std::string("EOF after end-tag-open: ") + error) ||
            !require(sink.tokens.size() == 1U, "end-tag-open EOF character token count") ||
            !require(
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                    sink.tokens[0].data == "</",
                "end-tag-open EOF literal recovery") ||
            !require(sink.errors.size() == 1U, "end-tag-open EOF error count") ||
            !require(
                sink.errors[0].code == "eof-before-tag-name" &&
                    sink.errors[0].column == 3U,
                "end-tag-open EOF error position")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("<<", {}, &sink, nullptr, &error),
                std::string("reconsumed tag-open EOF: ") + error) ||
            !require(sink.tokens.size() == 1U, "reconsumed tag-open EOF token count") ||
            !require(
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                    sink.tokens[0].data == "<<",
                "reconsumed tag-open EOF literal output") ||
            !require(sink.errors.size() == 2U, "reconsumed tag-open EOF error count") ||
            !require(
                sink.errors[0].code == "invalid-first-character-of-tag-name" &&
                    sink.errors[0].column == 2U,
                "reconsumed tag-open first error") ||
            !require(
                sink.errors[1].code == "eof-before-tag-name" &&
                    sink.errors[1].column == 3U,
                "reconsumed tag-open EOF error")) {
            return false;
        }
    }

    const std::vector<std::string> incomplete_tags = {
        "<a",
        "<a ",
        "<a a",
        "<a a ",
        "<a a =",
        "<a a =\"a",
        "<a a ='a",
        "<a a =a",
        "<a a ='a'",
        "<z/",
    };
    for (const std::string& input : incomplete_tags) {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                std::string("eof-in-tag recovery for ") + input + ": " + error) ||
            !require(sink.tokens.empty(), "incomplete tag publishes no token") ||
            !require(sink.errors.size() == 1U, "incomplete tag EOF error count") ||
            !require(
                sink.errors[0].code == "eof-in-tag" &&
                    sink.errors[0].line == 1U &&
                    sink.errors[0].column == input.size() + 1U,
                "incomplete tag EOF error position")) {
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
        "insert Data-tag EOF recovery tests",
    )
    text = replace_once(
        text,
        """        !test_tag_name_and_self_closing_state_recovery() ||\n        !test_end_tag_attributes_are_diagnosed_and_dropped() ||\n""",
        """        !test_tag_name_and_self_closing_state_recovery() ||\n        !test_tag_eof_recovery() ||\n        !test_end_tag_attributes_are_diagnosed_and_dropped() ||\n""",
        "register Data-tag EOF recovery tests",
    )
    TESTS.write_text(text, encoding="utf-8", newline="\n")


def patch_doc() -> None:
    text = DOC.read_text(encoding="utf-8")
    text = replace_once(
        text,
        """- self-closing-start-tag recovery: a `/` not followed by `>` emits `unexpected-solidus-in-tag` and reconsumes the following admitted ASCII byte in before-attribute-name, preserving input-stream error ordering;\n- tag-open invalid ASCII `anything else`: `invalid-first-character-of-tag-name`, literal `<` Character output, then reconsume in Data;\n""",
        """- self-closing-start-tag recovery: a `/` not followed by `>` emits `unexpected-solidus-in-tag` and reconsumes the following admitted ASCII byte in before-attribute-name, preserving input-stream error ordering;\n- EOF after tag-open or end-tag-open: `eof-before-tag-name`, with the literal `<` or `</` bytes emitted as Character data;\n- EOF in tag-name, attribute-name/value, after-attribute and self-closing-start-tag states: `eof-in-tag`, discarding the incomplete tag token without publishing partial attributes;\n- tag-open invalid ASCII `anything else`: `invalid-first-character-of-tag-name`, literal `<` Character output, then reconsume in Data;\n""",
        "document EOF recovery authority",
    )
    text = replace_once(
        text,
        """- non-ASCII raw-input preprocessing/location authority, including non-ASCII tag and attribute names;\n- generic EOF-in-tag/self-closing recovery and remaining malformed tag/attribute recovery not explicitly admitted above;\n- Script-data and CDATA states.\n""",
        """- non-ASCII raw-input preprocessing/location authority, including non-ASCII tag and attribute names;\n- remaining malformed tag/attribute recovery not explicitly admitted above;\n- Script-data and CDATA states.\n""",
        "remove admitted EOF family from fail-closed list",
    )
    DOC.write_text(text, encoding="utf-8", newline="\n")


def main() -> None:
    patch_source()
    patch_tests()
    patch_doc()


if __name__ == "__main__":
    main()
