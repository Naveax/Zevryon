#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(path: Path, old: str, new: str, label: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")


utf8_header = r'''#pragma once

#include <cstddef>
#include <string_view>

namespace zevryon::massivedoc::detail {

// Return the byte width of one well-formed Unicode scalar encoded as UTF-8 at
// offset. Zero means malformed/truncated UTF-8. ASCII is a one-byte scalar.
// This deliberately validates only UTF-8 scalar structure; HTML input-stream
// preprocessing (NUL replacement and CR/LF normalization) is a separate layer.
inline std::size_t html_tokenizer_utf8_scalar_bytes_v1(
    std::string_view input,
    std::size_t offset) noexcept {
    if (offset >= input.size()) {
        return 0U;
    }

    const auto byte = [&input](std::size_t index) noexcept {
        return static_cast<unsigned char>(input[index]);
    };
    const auto continuation = [&byte, &input](std::size_t index) noexcept {
        return index < input.size() && (byte(index) & 0xC0U) == 0x80U;
    };

    const unsigned char first = byte(offset);
    if (first <= 0x7FU) {
        return 1U;
    }
    if (first >= 0xC2U && first <= 0xDFU) {
        return continuation(offset + 1U) ? 2U : 0U;
    }
    if (first == 0xE0U) {
        if (offset + 2U >= input.size()) {
            return 0U;
        }
        const unsigned char second = byte(offset + 1U);
        return second >= 0xA0U && second <= 0xBFU && continuation(offset + 2U)
            ? 3U : 0U;
    }
    if ((first >= 0xE1U && first <= 0xECU) ||
        (first >= 0xEEU && first <= 0xEFU)) {
        return continuation(offset + 1U) && continuation(offset + 2U) ? 3U : 0U;
    }
    if (first == 0xEDU) {
        if (offset + 2U >= input.size()) {
            return 0U;
        }
        const unsigned char second = byte(offset + 1U);
        return second >= 0x80U && second <= 0x9FU && continuation(offset + 2U)
            ? 3U : 0U;
    }
    if (first == 0xF0U) {
        if (offset + 3U >= input.size()) {
            return 0U;
        }
        const unsigned char second = byte(offset + 1U);
        return second >= 0x90U && second <= 0xBFU &&
                continuation(offset + 2U) && continuation(offset + 3U)
            ? 4U : 0U;
    }
    if (first >= 0xF1U && first <= 0xF3U) {
        return continuation(offset + 1U) && continuation(offset + 2U) &&
                continuation(offset + 3U)
            ? 4U : 0U;
    }
    if (first == 0xF4U) {
        if (offset + 3U >= input.size()) {
            return 0U;
        }
        const unsigned char second = byte(offset + 1U);
        return second >= 0x80U && second <= 0x8FU &&
                continuation(offset + 2U) && continuation(offset + 3U)
            ? 4U : 0U;
    }
    return 0U;
}

} // namespace zevryon::massivedoc::detail
'''
(ROOT / "src/html_tokenizer_utf8_v1.hpp").write_text(utf8_header, encoding="utf-8", newline="\n")

# Data tokenizer: validate and preserve complete UTF-8 scalars in Data text.
data_cpp = ROOT / "src/html_tokenizer_data_tags_v1.cpp"
replace_once(
    data_cpp,
    '#include "html_tokenizer_character_reference_v1.hpp"\n',
    '#include "html_tokenizer_character_reference_v1.hpp"\n#include "html_tokenizer_utf8_v1.hpp"\n',
    "Data tokenizer UTF-8 include",
)
replace_once(
    data_cpp,
    '''    const std::size_t limit = std::min(offset, input.size());
    for (std::size_t index = 0U; index < limit; ++index) {
        if (input[index] == '\\n') {
            ++result.line;
            result.column = 1U;
        } else {
            ++result.column;
        }
    }
''',
    '''    const std::size_t limit = std::min(offset, input.size());
    for (std::size_t index = 0U; index < limit;) {
        if (input[index] == '\\n') {
            ++result.line;
            result.column = 1U;
            ++index;
            continue;
        }
        std::size_t scalar_bytes = 1U;
        if (!ascii_byte(input[index])) {
            const std::size_t validated =
                detail::html_tokenizer_utf8_scalar_bytes_v1(input, index);
            if (validated != 0U && validated <= limit - index) {
                scalar_bytes = validated;
            }
        }
        ++result.column;
        index += scalar_bytes;
    }
''',
    "Data tokenizer scalar-aware parse-error positions",
)
replace_once(
    data_cpp,
    '''            if (!ascii_byte(character)) {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer non-ASCII preprocessing/location authority is not implemented");
            }
            if (character == '&') {
''',
    '''            if (!ascii_byte(character)) {
                const std::size_t scalar_bytes =
                    detail::html_tokenizer_utf8_scalar_bytes_v1(input_, cursor);
                if (scalar_bytes == 0U) {
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer raw Data text contains invalid UTF-8 scalar encoding");
                }
                if (!append_bounded_bytes(
                        &character_buffer_,
                        input_.substr(cursor, scalar_bytes),
                        "HTML Data-tag tokenizer coalesced character token")) {
                    return false;
                }
                cursor += scalar_bytes;
                continue;
            }
            if (character == '&') {
''',
    "Data tokenizer raw UTF-8 Data dispatch",
)

# Character-reference diagnostics can occur after earlier UTF-8 Data scalars.
reference_cpp = ROOT / "src/html_tokenizer_character_reference_v1.cpp"
replace_once(
    reference_cpp,
    '#include "html_named_character_references_v1.generated.hpp"\n',
    '#include "html_named_character_references_v1.generated.hpp"\n#include "html_tokenizer_utf8_v1.hpp"\n',
    "character-reference UTF-8 include",
)
replace_once(
    reference_cpp,
    '''    const std::size_t limit = std::min(offset, input.size());
    for (std::size_t index = 0U; index < limit; ++index) {
        if (input[index] == '\\n') {
            ++result.line;
            result.column = 1U;
        } else {
            ++result.column;
        }
    }
''',
    '''    const std::size_t limit = std::min(offset, input.size());
    for (std::size_t index = 0U; index < limit;) {
        if (input[index] == '\\n') {
            ++result.line;
            result.column = 1U;
            ++index;
            continue;
        }
        std::size_t scalar_bytes = 1U;
        if (static_cast<unsigned char>(input[index]) >= 0x80U) {
            const std::size_t validated =
                detail::html_tokenizer_utf8_scalar_bytes_v1(input, index);
            if (validated != 0U && validated <= limit - index) {
                scalar_bytes = validated;
            }
        }
        ++result.column;
        index += scalar_bytes;
    }
''',
    "character-reference scalar-aware parse-error positions",
)

# The Data-stream coordinator must advance global source columns by scalars,
# not UTF-8 bytes, when a later delegated segment emits an error.
stream_cpp = ROOT / "src/html_tokenizer_data_stream_v1.cpp"
replace_once(
    stream_cpp,
    '#include "html_tokenizer_markup_declarations_v1.hpp"\n',
    '#include "html_tokenizer_markup_declarations_v1.hpp"\n#include "html_tokenizer_utf8_v1.hpp"\n',
    "Data stream UTF-8 include",
)
replace_once(
    stream_cpp,
    '''    for (char character : consumed) {
        if (character == '\\n') {
            if (position->line == std::numeric_limits<std::uint64_t>::max()) {
                return fail_stream(error, "HTML Data stream source line overflow");
            }
            ++position->line;
            position->column = 1U;
            continue;
        }
        if (position->column == std::numeric_limits<std::uint64_t>::max()) {
            return fail_stream(error, "HTML Data stream source column overflow");
        }
        ++position->column;
    }
''',
    '''    for (std::size_t index = 0U; index < consumed.size();) {
        const char character = consumed[index];
        if (character == '\\n') {
            if (position->line == std::numeric_limits<std::uint64_t>::max()) {
                return fail_stream(error, "HTML Data stream source line overflow");
            }
            ++position->line;
            position->column = 1U;
            ++index;
            continue;
        }
        std::size_t scalar_bytes = 1U;
        if (static_cast<unsigned char>(character) >= 0x80U) {
            scalar_bytes = detail::html_tokenizer_utf8_scalar_bytes_v1(consumed, index);
            if (scalar_bytes == 0U) {
                return fail_stream(
                    error,
                    "HTML Data stream source-position input contains invalid UTF-8 scalar encoding");
            }
        }
        if (position->column == std::numeric_limits<std::uint64_t>::max()) {
            return fail_stream(error, "HTML Data stream source column overflow");
        }
        ++position->column;
        index += scalar_bytes;
    }
''',
    "Data stream scalar-aware position advancement",
)

header = ROOT / "src/html_tokenizer_data_tags_v1.hpp"
replace_once(
    header,
    '''// reference table in Data and attribute-value contexts. Decoded references may
// produce UTF-8 output bytes even though raw input remains deliberately
// ASCII-only for v1 location/preprocessing authority.
//
// This slice intentionally does not approximate markup declarations,
// comments, DOCTYPE, NUL/input preprocessing, non-ASCII raw-input/location
// authority or the remaining broader malformed-
''',
    '''// reference table in Data and attribute-value contexts. Decoded references may
// produce UTF-8 output bytes. Raw Data character text additionally admits
// well-formed UTF-8 Unicode scalars and preserves their exact UTF-8 bytes;
// malformed UTF-8 fails closed. Parse-error columns across admitted UTF-8 Data
// text count Unicode scalars rather than encoding bytes.
//
// This slice intentionally does not approximate markup declarations,
// comments, DOCTYPE, NUL/input preprocessing, CR/LF preprocessing, non-ASCII
// tag/attribute names or raw non-ASCII attribute values, nor the broader malformed-
''',
    "Data tokenizer header UTF-8 contract",
)

cmake = ROOT / "cmake/logical_node_arena.cmake"
replace_once(
    cmake,
    '''  add_test(
    NAME html-tokenizer-data-tag-recovery-v1-tests
    COMMAND zevryon-html-tokenizer-data-tag-recovery-v1-tests)

  add_executable(
    zevryon-html-tokenizer-token-stream-v1-probe
''',
    '''  add_test(
    NAME html-tokenizer-data-tag-recovery-v1-tests
    COMMAND zevryon-html-tokenizer-data-tag-recovery-v1-tests)

  add_executable(
    zevryon-html-tokenizer-data-utf8-text-v1-tests
    tests/html_tokenizer_data_utf8_text_v1_tests.cpp)
  target_link_libraries(
    zevryon-html-tokenizer-data-utf8-text-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-html-tokenizer-data-utf8-text-v1-tests)
  add_test(
    NAME html-tokenizer-data-utf8-text-v1-tests
    COMMAND zevryon-html-tokenizer-data-utf8-text-v1-tests)

  add_executable(
    zevryon-html-tokenizer-token-stream-v1-probe
''',
    "CMake UTF-8 Data test target",
)

tests = r'''#include "html_tokenizer_data_tags_v1.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using zevryon::massivedoc::HtmlTokenizerDataTagsV1Config;
using zevryon::massivedoc::HtmlTokenizerDataTagsV1Stats;
using zevryon::massivedoc::HtmlTokenizerV1ParseError;
using zevryon::massivedoc::HtmlTokenizerV1Sink;
using zevryon::massivedoc::HtmlTokenizerV1Token;
using zevryon::massivedoc::HtmlTokenizerV1TokenKind;
using zevryon::massivedoc::tokenize_html_data_tags_v1;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: Data UTF-8 text v1: " << message << '\n';
        return false;
    }
    return true;
}

class CollectingSink final : public HtmlTokenizerV1Sink {
public:
    bool on_token(const HtmlTokenizerV1Token& token, std::string*) override {
        tokens.push_back(token);
        return true;
    }
    bool on_parse_error(const HtmlTokenizerV1ParseError& value, std::string*) override {
        errors.push_back(value);
        return true;
    }
    std::vector<HtmlTokenizerV1Token> tokens;
    std::vector<HtmlTokenizerV1ParseError> errors;
};

bool test_valid_scalar_passthrough() {
    const std::string input = std::string("A") +
        std::string("\xC2\xAC", 2U) +
        std::string("\xE2\x82\xAC", 3U) +
        std::string("\xF0\x9F\x98\x80", 4U) + "Z";
    CollectingSink sink;
    HtmlTokenizerDataTagsV1Stats stats;
    std::string error;
    return require(tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error), error) &&
        require(sink.tokens.size() == 1U, "valid UTF-8 emits one coalesced token") &&
        require(sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character,
                "valid UTF-8 emits Character token") &&
        require(sink.tokens[0].data == input, "valid UTF-8 bytes preserved exactly") &&
        require(sink.errors.empty(), "valid UTF-8 emits no diagnostics") &&
        require(stats.input_bytes == input.size(), "input byte accounting") &&
        require(stats.character_bytes_emitted == input.size(), "output byte accounting");
}

bool test_non_ascii_after_ampersand_is_literal_data() {
    const std::string input = std::string("&") + std::string("\xC2\xAC", 2U) + ";";
    CollectingSink sink;
    HtmlTokenizerDataTagsV1Stats stats;
    std::string error;
    return require(tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error), error) &&
        require(sink.tokens.size() == 1U &&
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                sink.tokens[0].data == input,
                "non-name Unicode scalar after ampersand remains literal") &&
        require(sink.errors.empty() && stats.parse_errors_emitted == 0U,
                "literal Unicode ampersand fallback diagnostics");
}

bool test_scalar_aware_error_columns() {
    const std::string input = std::string("\xC2\xAC", 2U) + "<q a=`>";
    CollectingSink sink;
    HtmlTokenizerDataTagsV1Stats stats;
    std::string error;
    return require(tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error), error) &&
        require(sink.errors.size() == 1U, "Unicode-prefix parse-error count") &&
        require(sink.errors[0].code == "unexpected-character-in-unquoted-attribute-value",
                "Unicode-prefix parse-error code") &&
        require(sink.errors[0].line == 1U && sink.errors[0].column == 7U,
                "Unicode-prefix column counts scalar, not UTF-8 bytes");
}

bool test_invalid_utf8_fails_closed() {
    const std::vector<std::string> invalid = {
        std::string("\x80", 1U),
        std::string("\xC0\x80", 2U),
        std::string("\xC2", 1U),
        std::string("\xC2\x41", 2U),
        std::string("\xE0\x80\x80", 3U),
        std::string("\xED\xA0\x80", 3U),
        std::string("\xF0\x80\x80\x80", 4U),
        std::string("\xF4\x90\x80\x80", 4U),
        std::string("\xF5\x80\x80\x80", 4U),
    };
    for (const std::string& input : invalid) {
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Stats stats;
        std::string error;
        if (!require(!tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error),
                     "malformed UTF-8 must fail") ||
            !require(error.find("invalid UTF-8 scalar encoding") != std::string::npos,
                     "malformed UTF-8 failure is explicit") ||
            !require(sink.tokens.empty() && sink.errors.empty(),
                     "malformed UTF-8 publishes no partial events")) {
            return false;
        }
    }
    return true;
}

bool test_utf8_respects_token_byte_cap() {
    CollectingSink sink;
    HtmlTokenizerDataTagsV1Stats stats;
    HtmlTokenizerDataTagsV1Config config;
    config.maximum_token_bytes = 1U;
    std::string error;
    const std::string input("\xC2\xAC", 2U);
    return require(!tokenize_html_data_tags_v1(input, config, &sink, &stats, &error),
                   "two-byte scalar exceeds one-byte token cap") &&
        require(error.find("coalesced character token exceeds bounded byte limit") != std::string::npos,
                "UTF-8 cap failure remains explicit") &&
        require(sink.tokens.empty(), "UTF-8 cap failure publishes no token");
}

bool test_scope_guards_remain_fail_closed() {
    const std::string scalar("\xC2\xAC", 2U);
    {
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Stats stats;
        std::string error;
        const std::string input = "<" + scalar + ">";
        if (!require(!tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error),
                     "non-ASCII tag name remains outside slice") ||
            !require(error.find("non-ASCII preprocessing/location authority") != std::string::npos,
                     "non-ASCII tag-name guard remains explicit")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Stats stats;
        std::string error;
        const std::string input = "<h a='" + scalar + "'>";
        if (!require(!tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error),
                     "non-ASCII attribute value remains outside slice") ||
            !require(error.find("non-ASCII attribute-value authority") != std::string::npos,
                     "non-ASCII attribute-value guard remains explicit")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Stats stats;
        std::string error;
        const std::string input("A\0B", 3U);
        if (!require(!tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error),
                     "NUL preprocessing remains outside slice") ||
            !require(error.find("NUL replacement is not implemented") != std::string::npos,
                     "NUL guard remains explicit")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    return test_valid_scalar_passthrough() &&
            test_non_ascii_after_ampersand_is_literal_data() &&
            test_scalar_aware_error_columns() &&
            test_invalid_utf8_fails_closed() &&
            test_utf8_respects_token_byte_cap() &&
            test_scope_guards_remain_fail_closed()
        ? 0
        : 1;
}
'''
(ROOT / "tests/html_tokenizer_data_utf8_text_v1_tests.cpp").write_text(
    tests, encoding="utf-8", newline="\n")

doc = r'''# Z7 HTML tokenizer Data UTF-8 text v1

## Purpose

This production slice admits well-formed UTF-8 Unicode scalar sequences as ordinary character data in the existing bounded Data tokenizer. It closes the raw-Data-text gap needed for literal non-ASCII characters while keeping broader HTML input preprocessing and non-ASCII markup surfaces fail-closed.

## Admitted behavior

- ASCII Data behavior remains unchanged.
- A non-ASCII Data byte must begin one canonical well-formed UTF-8 scalar sequence.
- 2-, 3- and 4-byte scalar encodings are validated for continuation structure, overlong encodings, surrogate exclusion and the U+10FFFF upper bound.
- Valid scalar bytes are preserved byte-for-byte in the coalesced Character token.
- The existing `maximum_token_bytes` bound applies to the complete UTF-8 byte sequence before publication.
- Character-reference fallback followed by a non-name Unicode scalar remains literal Data, including the pinned html5lib `&¬;` shape.
- Parse-error columns in the Data tokenizer and character-reference diagnostics count admitted UTF-8 scalars instead of encoding bytes.
- The Data-stream coordinator advances delegated source columns by admitted UTF-8 scalars so later segment diagnostics keep the same coordinate basis.

## Fail-closed boundary

This slice still rejects:

- malformed, truncated, overlong, surrogate or out-of-range UTF-8;
- raw U+0000 / NUL preprocessing;
- CR/LF input-stream normalization;
- non-ASCII tag names;
- non-ASCII attribute names;
- raw non-ASCII attribute values;
- any broader tokenizer state or preprocessing behavior not already admitted.

The scalar validator is intentionally an encoding boundary, not a substitute for the WHATWG input-stream preprocessing algorithm.

## Verification

The dedicated `html-tokenizer-data-utf8-text-v1-tests` target freezes valid 2/3/4-byte passthrough, literal ampersand fallback, scalar-aware error columns, malformed UTF-8 rejection, token-byte bounds, and the retained non-ASCII markup/NUL guards.

The production probe is additionally checked with the exact UTF-8 bytes for `&¬;` so this slice can later support a separate external-authority promotion without coupling production behavior to the authority runner.

## Nonclaims

This slice does not by itself claim complete html5lib `test1.test`, complete input-stream preprocessing, arbitrary non-ASCII markup support, full WHATWG tokenizer conformance, `html_tokenizer_conformance`, tree-builder conformance, or Z7 completion.
'''
(ROOT / "docs/Z7_HTML_TOKENIZER_DATA_UTF8_TEXT_V1.md").write_text(
    doc, encoding="utf-8", newline="\n")

print("prepared bounded Data UTF-8 text v1 production slice")
