#!/usr/bin/env python3
from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"expected exactly one correction anchor in {path}, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


cpp = Path("src/html_tokenizer_data_tags_v1.cpp")
replace_once(
    cpp,
    '''    bool consume_bogus_comment(
        std::size_t* cursor,
        std::size_t data_begin,
        std::size_t error_offset,
        std::string error_code) {''',
    '''    bool consume_bogus_comment(
        std::size_t* cursor,
        std::size_t data_begin,
        std::size_t error_offset,
        std::string error_code,
        bool leading_control_error_emitted) {''',
)
replace_once(
    cpp,
    '''            if (ascii_control_parse_error(character) &&
                !emit_parse_error(scan, "control-character-in-input-stream")) {''',
    '''            if (ascii_control_parse_error(character) &&
                !(leading_control_error_emitted && scan == data_begin) &&
                !emit_parse_error(scan, "control-character-in-input-stream")) {''',
)
replace_once(
    cpp,
    '''            return consume_bogus_comment(
                cursor,
                probe,
                probe,
                "unexpected-question-mark-instead-of-tag-name");''',
    '''            return consume_bogus_comment(
                cursor,
                probe,
                probe,
                "unexpected-question-mark-instead-of-tag-name",
                false);''',
)
replace_once(
    cpp,
    '''            if (end_tag) {
                return consume_bogus_comment(
                    cursor,
                    probe,
                    probe,
                    "invalid-first-character-of-tag-name");
            }''',
    '''            if (end_tag) {
                const bool leading_control_error_emitted =
                    ascii_control_parse_error(input_[probe]);
                if (leading_control_error_emitted &&
                    !emit_parse_error(probe, "control-character-in-input-stream")) {
                    return false;
                }
                return consume_bogus_comment(
                    cursor,
                    probe,
                    probe,
                    "invalid-first-character-of-tag-name",
                    leading_control_error_emitted);
            }''',
)

tests = Path("tests/html_tokenizer_data_tags_v1_tests.cpp")
anchor = '''    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("a<?b>c", {}, &sink, nullptr, &error),'''
regression = r'''    {
        CollectingSink sink;
        std::string error;
        const std::string input("</\v>", 4U);
        if (!require(
                tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                std::string("invalid end-tag control ordering: ") + error) ||
            !require(sink.tokens.size() == 1U, "invalid end-tag control comment count") ||
            !require(
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Comment &&
                    sink.tokens[0].data == std::string("\v", 1U),
                "invalid end-tag control comment payload") ||
            !require(sink.errors.size() == 2U, "invalid end-tag control error count") ||
            !require(
                sink.errors[0].code == "control-character-in-input-stream" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == 3U,
                "input-stream control error precedes end-tag state error") ||
            !require(
                sink.errors[1].code == "invalid-first-character-of-tag-name" &&
                    sink.errors[1].line == 1U && sink.errors[1].column == 3U,
                "invalid end-tag state error follows control observation")) {
            return false;
        }
    }
'''
replace_once(tests, anchor, regression + anchor)

print("aligned invalid end-tag control error ordering")
