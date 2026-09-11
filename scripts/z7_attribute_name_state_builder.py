#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def write(path: str, text: str) -> None:
    (ROOT / path).write_text(text, encoding="utf-8")


def replace_once(path: str, old: str, new: str) -> None:
    text = read(path)
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one replacement site, found {count}")
    write(path, text.replace(old, new, 1))


def replace_region(path: str, start: str, end: str, replacement: str) -> None:
    text = read(path)
    start_index = text.find(start)
    if start_index < 0:
        raise RuntimeError(f"{path}: start marker not found: {start!r}")
    end_index = text.find(end, start_index)
    if end_index < 0:
        raise RuntimeError(f"{path}: end marker not found: {end!r}")
    if text.find(start, start_index + len(start)) >= 0:
        raise RuntimeError(f"{path}: start marker is not unique")
    write(path, text[:start_index] + replacement + text[end_index:])


SOURCE = "src/html_tokenizer_data_tags_v1.cpp"
TEST = "tests/html_tokenizer_data_tags_v1_tests.cpp"
DOC = "docs/Z7_HTML_TOKENIZER_DATA_TAGS_V1.md"

replace_once(
    SOURCE,
    """bool ascii_space(char value) noexcept {
    return value == ' ' || value == '\\t' || value == '\\n' ||
        value == '\\r' || value == '\\f';
}

char ascii_lower(char value) noexcept {
""",
    """bool ascii_space(char value) noexcept {
    return value == ' ' || value == '\\t' || value == '\\n' ||
        value == '\\r' || value == '\\f';
}

bool ascii_control_parse_error(char value) noexcept {
    const auto byte = static_cast<unsigned char>(value);
    return (byte >= 0x01U && byte <= 0x08U) || byte == 0x0BU ||
        (byte >= 0x0EU && byte <= 0x1FU) || byte == 0x7FU;
}

char ascii_lower(char value) noexcept {
""",
)

new_parse_attribute = r'''    bool parse_attribute(
        std::size_t* cursor,
        std::vector<HtmlTokenizerV1Attribute>* attributes,
        std::size_t* token_bytes,
        bool missing_whitespace_before) {
        if (*cursor >= input_.size()) {
            return fail_data_tokenizer(
                error_,
                "HTML Data-tag tokenizer EOF before attribute name is outside admitted recovery");
        }

        const std::size_t name_begin = *cursor;
        HtmlTokenizerV1Attribute attribute;
        bool leading_control_error_emitted = false;

        // Input-stream control errors conceptually occur while the next input
        // character is consumed. Preserve that ordering ahead of the tokenizer
        // missing-whitespace diagnostic when the same byte starts an attribute.
        if (missing_whitespace_before &&
            ascii_byte(input_[*cursor]) &&
            ascii_control_parse_error(input_[*cursor])) {
            if (!emit_parse_error(*cursor, "control-character-in-input-stream")) {
                return false;
            }
            leading_control_error_emitted = true;
        }
        if (missing_whitespace_before &&
            !emit_parse_error(*cursor, "missing-whitespace-between-attributes")) {
            return false;
        }

        // WHATWG before-attribute-name '=' recovery starts a real attribute
        // whose initial name is '=' and then enters the attribute-name state.
        if (input_[*cursor] == '=') {
            if (!emit_parse_error(
                    *cursor,
                    "unexpected-equals-sign-before-attribute-name") ||
                !append_name_character(&attribute.name, '=')) {
                return false;
            }
            ++*cursor;
        } else {
            while (*cursor < input_.size()) {
                const char character = input_[*cursor];
                if (ascii_space(character) || character == '/' ||
                    character == '>' || character == '=') {
                    break;
                }
                if (character == '\0') {
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");
                }
                if (!ascii_byte(character)) {
                    // Keep the historical v3 census classification stable until
                    // Unicode preprocessing/location authority is admitted.
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer attribute name byte is outside admitted v1 subset");
                }
                if (ascii_control_parse_error(character) &&
                    !(leading_control_error_emitted && *cursor == name_begin) &&
                    !emit_parse_error(*cursor, "control-character-in-input-stream")) {
                    return false;
                }
                if ((character == '"' || character == '\'' || character == '<') &&
                    !emit_parse_error(
                        *cursor,
                        "unexpected-character-in-attribute-name")) {
                    return false;
                }
                if (!append_name_character(&attribute.name, character)) {
                    return false;
                }
                ++*cursor;
            }
        }

        if (attribute.name.empty()) {
            return fail_data_tokenizer(
                error_,
                "HTML Data-tag tokenizer attribute name byte is outside admitted v1 subset");
        }

        const bool duplicate = std::any_of(
            attributes->begin(),
            attributes->end(),
            [&attribute](const HtmlTokenizerV1Attribute& existing) {
                return existing.name == attribute.name;
            });
        if (duplicate &&
            !emit_parse_error(*cursor, "duplicate-attribute")) {
            return false;
        }

        const std::size_t after_name = *cursor;
        std::size_t after_whitespace = after_name;
        while (after_whitespace < input_.size() &&
               ascii_space(input_[after_whitespace])) {
            ++after_whitespace;
        }
        if (after_whitespace < input_.size() &&
            input_[after_whitespace] == '=') {
            *cursor = after_whitespace + 1U;
            while (*cursor < input_.size() && ascii_space(input_[*cursor])) {
                ++*cursor;
            }
            if (*cursor < input_.size() && input_[*cursor] == '>') {
                if (!emit_parse_error(*cursor, "missing-attribute-value")) {
                    return false;
                }
            } else if (!parse_attribute_value(cursor, &attribute.value)) {
                return false;
            }
        } else {
            // Leave separator whitespace for the outer before-attribute loop so
            // the next attribute is not misdiagnosed as missing whitespace.
            *cursor = after_name;
        }

        if (!token_budget_accepts(*token_bytes, attribute.name.size()) ||
            !token_budget_accepts(
                *token_bytes + attribute.name.size(),
                attribute.value.size())) {
            return fail_data_tokenizer(
                error_,
                "HTML Data-tag tokenizer tag token exceeds bounded byte limit");
        }

        if (!duplicate) {
            if (attributes->size() >= config_.maximum_attributes) {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer attribute count exceeds bounded limit");
            }
            *token_bytes += attribute.name.size();
            *token_bytes += attribute.value.size();
            attributes->push_back(std::move(attribute));
        }
        return true;
    }

'''
replace_region(SOURCE, "    bool parse_attribute(\n", "    bool emit_tag(\n", new_parse_attribute)

replace_once(
    SOURCE,
    """            if (!had_whitespace) {
                if (!parsed_attribute) {
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer first attribute without separating whitespace is outside admitted v1 subset");
                }
                if (!emit_parse_error(
                        probe,
                        "missing-whitespace-between-attributes")) {
                    return false;
                }
            }

            if (!parse_attribute(
                    &probe,
                    &attributes,
                    &token_bytes)) {
""",
    """            bool missing_whitespace_before = false;
            if (!had_whitespace) {
                if (!parsed_attribute) {
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer first attribute without separating whitespace is outside admitted v1 subset");
                }
                missing_whitespace_before = true;
            }

            if (!parse_attribute(
                    &probe,
                    &attributes,
                    &token_bytes,
                    missing_whitespace_before)) {
""",
)

new_test = r'''bool test_attribute_name_state_recovery() {
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("<z =>", {}, &sink, nullptr, &error),
                std::string("equals-name recovery: ") + error)) {
            return false;
        }
        if (!require(sink.tokens.size() == 1U, "equals-name token count") ||
            !require(sink.tokens[0].attributes.size() == 1U, "equals-name attribute count") ||
            !require(attribute_is(sink.tokens[0].attributes[0], "=", ""), "equals-name payload") ||
            !require(sink.errors.size() == 1U, "equals-name error count") ||
            !require(
                sink.errors[0].code == "unexpected-equals-sign-before-attribute-name" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == 4U,
                "equals-name error position")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("<z ==> ", {}, &sink, nullptr, &error) == false,
                "sentinel malformed case remains bounded")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("<z ==>", {}, &sink, nullptr, &error),
                std::string("missing attribute value recovery: ") + error)) {
            return false;
        }
        if (!require(sink.tokens.size() == 1U, "missing-value token count") ||
            !require(sink.tokens[0].attributes.size() == 1U, "missing-value attribute count") ||
            !require(attribute_is(sink.tokens[0].attributes[0], "=", ""), "missing-value payload") ||
            !require(sink.errors.size() == 2U, "missing-value error count") ||
            !require(
                sink.errors[0].code == "unexpected-equals-sign-before-attribute-name" &&
                    sink.errors[0].column == 4U,
                "missing-value first error") ||
            !require(
                sink.errors[1].code == "missing-attribute-value" &&
                    sink.errors[1].column == 6U,
                "missing-value second error")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("<foo \"='bar'>", {}, &sink, nullptr, &error),
                std::string("quoted-byte attribute name: ") + error)) {
            return false;
        }
        if (!require(sink.tokens.size() == 1U, "quoted-name token count") ||
            !require(sink.tokens[0].attributes.size() == 1U, "quoted-name attribute count") ||
            !require(attribute_is(sink.tokens[0].attributes[0], "\"", "bar"), "quoted-name payload") ||
            !require(sink.errors.size() == 1U, "quoted-name error count") ||
            !require(
                sink.errors[0].code == "unexpected-character-in-attribute-name" &&
                    sink.errors[0].column == 6U,
                "quoted-name error position")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string input = "<a a=''";
        input.push_back('\x0B');
        input.push_back('>');
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                std::string("control attribute name: ") + error)) {
            return false;
        }
        const std::string control_name(1U, '\x0B');
        if (!require(sink.tokens.size() == 1U, "control-name token count") ||
            !require(sink.tokens[0].attributes.size() == 2U, "control-name attribute count") ||
            !require(attribute_is(sink.tokens[0].attributes[0], "a", ""), "control-name first attribute") ||
            !require(attribute_is(sink.tokens[0].attributes[1], control_name, ""), "control-name payload") ||
            !require(sink.errors.size() == 2U, "control-name error count") ||
            !require(
                sink.errors[0].code == "control-character-in-input-stream" &&
                    sink.errors[0].column == 8U,
                "control-name input error order") ||
            !require(
                sink.errors[1].code == "missing-whitespace-between-attributes" &&
                    sink.errors[1].column == 8U,
                "control-name whitespace error order")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("<a a<>", {}, &sink, nullptr, &error),
                std::string("less-than attribute name: ") + error)) {
            return false;
        }
        return require(sink.tokens.size() == 1U, "less-than token count") &&
            require(sink.tokens[0].attributes.size() == 1U, "less-than attribute count") &&
            require(attribute_is(sink.tokens[0].attributes[0], "a<", ""), "less-than name retained") &&
            require(sink.errors.size() == 1U, "less-than error count") &&
            require(
                sink.errors[0].code == "unexpected-character-in-attribute-name" &&
                    sink.errors[0].column == 5U,
                "less-than error position");
    }
}

'''
replace_once(
    TEST,
    "bool test_admitted_references_and_fail_closed_boundaries() {\n",
    new_test + "bool test_admitted_references_and_fail_closed_boundaries() {\n",
)
replace_once(
    TEST,
    """        !test_duplicate_attribute_first_wins() ||
        !test_end_tag_attributes_are_diagnosed_and_dropped() ||
""",
    """        !test_duplicate_attribute_first_wins() ||
        !test_attribute_name_state_recovery() ||
        !test_end_tag_attributes_are_diagnosed_and_dropped() ||
""",
)

replace_once(
    DOC,
    "- ASCII-case-insensitive tag and attribute names normalized to lowercase;\n",
    "- ASCII-case-insensitive tag names plus bounded ASCII attribute-name state handling, with ASCII letters normalized to lowercase;\n",
)
replace_once(
    DOC,
    """- `missing-whitespace-between-attributes`, retaining both admitted attributes;
- `duplicate-attribute`, retaining the first value and dropping the duplicate;
""",
    """- `missing-whitespace-between-attributes`, retaining both admitted attributes;
- before-attribute-name `=` recovery: `unexpected-equals-sign-before-attribute-name`, creating an attribute whose initial name byte is `=`;
- attribute-name `\"`, `'` and `<`: `unexpected-character-in-attribute-name`, with the offending ASCII byte retained;
- admitted C0/DEL attribute-name input controls: `control-character-in-input-stream`, preserving the byte and maintaining input-error ordering ahead of tokenizer recovery on the same byte;
- `missing-attribute-value` when an equals delimiter reaches `>` before a value;
- ordinary admitted ASCII punctuation in attribute names, while `/`, whitespace, `>` and `=` retain their tokenizer transition roles;
- `duplicate-attribute`, retaining the first value and dropping the duplicate;
""",
)
replace_once(
    DOC,
    "- non-ASCII raw-input preprocessing/location authority;\n",
    "- non-ASCII raw-input preprocessing/location authority, including non-ASCII attribute names;\n",
)

print("attribute-name production patch staged")
