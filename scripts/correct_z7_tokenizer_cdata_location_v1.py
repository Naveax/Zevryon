#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"expected exactly one correction anchor in {path}, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


cpp = ROOT / "src/html_tokenizer_token_stream_v1.cpp"
replace_once(
    cpp,
    '#include "html_tokenizer_script_data_v1.hpp"\n',
    '#include "html_tokenizer_script_data_v1.hpp"\n#include "html_tokenizer_utf8_v1.hpp"\n',
)

anchor = """bool emit_canonical_parse_error(
    std::string_view full_input,
"""
helper = r'''HtmlTokenizerV1ParseError cdata_position_error_utf16(
    std::string_view input,
    std::size_t offset,
    std::string code) {
    HtmlTokenizerV1ParseError result;
    result.code = std::move(code);
    result.line = 1U;
    result.column = 1U;

    const std::size_t limit = std::min(offset, input.size());
    std::size_t index = 0U;
    while (index < limit) {
        if (input[index] == '\n') {
            ++result.line;
            result.column = 1U;
            ++index;
            continue;
        }

        const std::size_t scalar_bytes =
            detail::html_tokenizer_utf8_scalar_bytes_v1(input, index);
        if (scalar_bytes == 0U || scalar_bytes > limit - index) {
            // Invalid UTF-8 never reaches the admitted CDATA corpus surface;
            // keep this fallback bounded and monotonic rather than reading
            // across an invalid/truncated sequence.
            ++result.column;
            ++index;
            continue;
        }

        // html5lib's pinned error coordinates count UTF-16 code units.
        // A well-formed four-byte UTF-8 scalar is supplementary and therefore
        // occupies one surrogate pair; all other Unicode scalars occupy one.
        result.column += scalar_bytes == 4U ? 2U : 1U;
        index += scalar_bytes;
    }
    return result;
}

'''
replace_once(cpp, anchor, helper + anchor)
replace_once(
    cpp,
    """    HtmlTokenizerV1ParseError parse_error =
        position_error(full_input, offset, std::move(code));
""",
    """    HtmlTokenizerV1ParseError parse_error =
        cdata_position_error_utf16(full_input, offset, std::move(code));
""",
)
replace_once(
    cpp,
    """    const HtmlTokenizerV1ParseError base =
        position_error(input, data_offset, "");
    CdataContinuationSink translated_sink(
""",
    """    const HtmlTokenizerV1ParseError base =
        cdata_position_error_utf16(input, data_offset, "");
    CdataContinuationSink translated_sink(
""",
)

tests = ROOT / "tests/html_tokenizer_token_stream_v1_tests.cpp"
test_anchor = """        !run_case(
            "CDATA control then EOF",
"""
regressions = r'''        !run_case(
            "CDATA supplementary scalar EOF coordinate",
            HtmlTokenizerV1InitialState::CdataSection,
            "",
            std::string("\xF4\x80\x80\x80", 4U),
            {character(std::string("\xF4\x80\x80\x80", 4U))},
            {ExpectedError{"eof-in-cdata", 1U, 3U}}) ||
        !run_case(
            "CDATA ASCII plus supplementary scalar EOF coordinate",
            HtmlTokenizerV1InitialState::CdataSection,
            "",
            std::string(";\xF4\x80\x80\x80", 5U),
            {character(std::string(";\xF4\x80\x80\x80", 5U))},
            {ExpectedError{"eof-in-cdata", 1U, 4U}}) ||
'''
replace_once(tests, test_anchor, regressions + test_anchor)

print("corrected CDATA UTF-16 error coordinates")
