#!/usr/bin/env python3
from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    target = Path(path)
    text = target.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one replacement site, found {count}")
    target.write_text(text.replace(old, new), encoding="utf-8")


replace_once(
    "src/html_tokenizer_character_reference_v1.cpp",
    '#include "html_tokenizer_character_reference_v1.hpp"\n\n#include <algorithm>',
    '#include "html_tokenizer_character_reference_v1.hpp"\n\n#include "html_named_character_references_v1.generated.hpp"\n\n#include <algorithm>',
)

NAMED_HELPERS = r'''const HtmlNamedCharacterReferenceV1GeneratedEntry* find_longest_named_reference(
    std::string_view input,
    std::size_t name_start,
    std::size_t* matched_length) noexcept {
    *matched_length = 0U;
    if (name_start >= input.size()) {
        return nullptr;
    }

    const std::size_t maximum = std::min<std::size_t>(
        kHtmlNamedCharacterReferenceV1MaximumNameBytes,
        input.size() - name_start);
    for (std::size_t length = maximum; length > 0U; --length) {
        const std::string_view candidate = input.substr(name_start, length);
        const auto found = std::lower_bound(
            kHtmlNamedCharacterReferenceV1Entries.begin(),
            kHtmlNamedCharacterReferenceV1Entries.end(),
            candidate,
            [](const HtmlNamedCharacterReferenceV1GeneratedEntry& entry,
               std::string_view value) noexcept {
                return entry.name < value;
            });
        if (found != kHtmlNamedCharacterReferenceV1Entries.end() &&
            found->name == candidate) {
            *matched_length = length;
            return &*found;
        }
    }
    return nullptr;
}

bool consume_named_reference(
    std::string_view input,
    std::size_t ampersand_offset,
    HtmlTokenizerCharacterReferenceV1Context context,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerCharacterReferenceV1Stats* stats,
    HtmlTokenizerCharacterReferenceV1Result* result,
    std::string* error) {
    const std::size_t name_start = ampersand_offset + 1U;
    std::size_t matched_length = 0U;
    const HtmlNamedCharacterReferenceV1GeneratedEntry* matched =
        find_longest_named_reference(input, name_start, &matched_length);

    if (matched == nullptr) {
        std::size_t cursor = name_start;
        while (cursor < input.size() && ascii_alphanumeric(input[cursor])) {
            ++cursor;
        }
        if (cursor < input.size() && input[cursor] == ';') {
            if (!emit_parse_error(
                    input,
                    cursor,
                    "unknown-named-character-reference",
                    sink,
                    stats,
                    error)) {
                return false;
            }
        }
        result->replacement_utf8.assign(
            input.substr(ampersand_offset, cursor - ampersand_offset));
        result->next_offset = cursor;
        return true;
    }

    const std::size_t matched_end = name_start + matched_length;
    const bool terminated = !matched->name.empty() && matched->name.back() == ';';
    if (!terminated &&
        context == HtmlTokenizerCharacterReferenceV1Context::Attribute &&
        matched_end < input.size() &&
        (input[matched_end] == '=' || ascii_alphanumeric(input[matched_end]))) {
        result->replacement_utf8.assign(
            input.substr(ampersand_offset, matched_end - ampersand_offset));
        result->next_offset = matched_end;
        return true;
    }

    if (!terminated &&
        !emit_parse_error(
            input,
            matched_end,
            "missing-semicolon-after-character-reference",
            sink,
            stats,
            error)) {
        return false;
    }

    if (matched->codepoint_count == 0U || matched->codepoint_count > 2U) {
        return fail_reference(error, "HTML named-reference table entry is invalid");
    }
    result->replacement_utf8.clear();
    if (!append_utf8(matched->first_codepoint, &result->replacement_utf8, error)) {
        return false;
    }
    if (matched->codepoint_count == 2U &&
        !append_utf8(matched->second_codepoint, &result->replacement_utf8, error)) {
        return false;
    }
    result->next_offset = matched_end;
    return true;
}

'''
replace_once(
    "src/html_tokenizer_character_reference_v1.cpp",
    "bool consume_impl(\n",
    NAMED_HELPERS + "bool consume_impl(\n",
)
replace_once(
    "src/html_tokenizer_character_reference_v1.cpp",
    '''    if (ascii_alphanumeric(first)) {
        return fail_reference(
            error,
            "HTML named character references are outside admitted numeric v1 subset");
    }
''',
    '''    if (ascii_alphanumeric(first)) {
        return consume_named_reference(
            input,
            ampersand_offset,
            context,
            sink,
            stats,
            result,
            error);
    }
''',
)

replace_once(
    "src/html_tokenizer_character_reference_v1.hpp",
    '''//   * decimal and hexadecimal numeric character references;
//   * absence-of-digits and missing-semicolon recovery;
//   * the WHATWG numeric-reference end-state scalar/control/noncharacter rules.
//
// Named-reference candidates remain fail-closed until a separately pinned
// complete named-reference table is admitted. The context is already explicit
// because named-reference behavior differs inside attributes.
''',
    '''//   * decimal and hexadecimal numeric character references;
//   * the complete pinned WHATWG named-character-reference table with bounded
//     maximum-length matching, one/two-scalar UTF-8 replacement, legacy
//     semicolon recovery and attribute-context historical veto;
//   * ambiguous-ampersand literal recovery and unknown-name diagnostics;
//   * the WHATWG numeric-reference end-state scalar/control/noncharacter rules.
//
// The context is explicit because semicolonless legacy named-reference behavior
// differs inside attributes. Raw non-ASCII input preprocessing remains outside
// this v1 character-reference component's authority.
''',
)

replace_once(
    "src/html_tokenizer_data_tags_v1.hpp",
    '''// Character-reference admission includes literal ampersand fallback plus
// bounded decimal/hex numeric references in Data and attribute-value contexts.
// Numeric references may produce UTF-8 output bytes even though raw input
// remains deliberately ASCII-only for v1 location/preprocessing authority.
// Complete named references remain fail closed.
''',
    '''// Character-reference admission includes literal ampersand fallback,
// bounded decimal/hex numeric references and the complete pinned WHATWG named
// reference table in Data and attribute-value contexts. Decoded references may
// produce UTF-8 output bytes even though raw input remains deliberately
// ASCII-only for v1 location/preprocessing authority.
''',
)
replace_once(
    "src/html_tokenizer_data_tags_v1.hpp",
    '''// This slice intentionally does not approximate markup declarations,
// comments, DOCTYPE, named character references, NUL/input preprocessing,
// non-ASCII raw-input/location authority or the remaining broader malformed-
''',
    '''// This slice intentionally does not approximate markup declarations,
// comments, DOCTYPE, NUL/input preprocessing, non-ASCII raw-input/location
// authority or the remaining broader malformed-
''',
)

replace_once(
    "src/html_tokenizer_token_stream_v1.hpp",
    '''// bounded Data-stream coordinator, including literal ampersand fallback and
// bounded decimal/hex numeric character references in Data/attribute contexts.
''',
    '''// bounded Data-stream coordinator, including literal ampersand fallback,
// bounded decimal/hex numeric references and the complete pinned WHATWG named
// character-reference table in Data/attribute contexts.
''',
)
replace_once(
    "src/html_tokenizer_token_stream_v1.hpp",
    '''// This remains intentionally narrower than complete WHATWG tokenization.
// CDATA, complete named character references, full input-stream preprocessing
// and broader recovery remain fail-closed where the admitted component
// surfaces do not yet implement them.
''',
    '''// This remains intentionally narrower than complete WHATWG tokenization.
// CDATA, full input-stream preprocessing/non-ASCII raw-input location authority
// and broader recovery remain fail-closed where the admitted component surfaces
// do not yet implement them.
''',
)

replace_once(
    "tests/html_tokenizer_numeric_character_reference_v1_tests.cpp",
    '''bool test_named_candidate_remains_fail_closed() {
    CollectingSink sink;
    HtmlTokenizerCharacterReferenceV1Stats stats;
    HtmlTokenizerCharacterReferenceV1Result result;
    std::string error;
    return require(
               !run_reference(
                   "&not;",
                   HtmlTokenizerCharacterReferenceV1Context::Data,
                   &sink,
                   &stats,
                   &result,
                   &error),
               "named reference must remain unsupported") &&
        require(error.find("named character references") != std::string::npos,
                "named failure explicit") &&
        require(sink.errors.empty() && sink.tokens.empty(), "named failure publishes nothing") &&
        require(stats.parse_errors_emitted == 0U, "named failure stats");
}
''',
    '''bool test_named_candidate_uses_shared_table() {
    CollectingSink sink;
    HtmlTokenizerCharacterReferenceV1Stats stats;
    HtmlTokenizerCharacterReferenceV1Result result;
    std::string error;
    const std::string not_sign("\\xC2\\xAC", 2U);
    return require(
               run_reference(
                   "&not;",
                   HtmlTokenizerCharacterReferenceV1Context::Data,
                   &sink,
                   &stats,
                   &result,
                   &error),
               std::string("named reference dispatch: ") + error) &&
        require(result.replacement_utf8 == not_sign && result.next_offset == 5U,
                "named reference replacement") &&
        require(sink.errors.empty() && sink.tokens.empty(), "named reference diagnostics") &&
        require(stats.parse_errors_emitted == 0U, "named reference stats");
}
''',
)
replace_once(
    "tests/html_tokenizer_numeric_character_reference_v1_tests.cpp",
    "            test_named_candidate_remains_fail_closed() &&\n",
    "            test_named_candidate_uses_shared_table() &&\n",
)

CMAKE = '''target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/html_tokenizer_character_reference_v1.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-html-tokenizer-numeric-character-reference-v1-tests
    tests/html_tokenizer_numeric_character_reference_v1_tests.cpp)
  target_link_libraries(
    zevryon-html-tokenizer-numeric-character-reference-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-html-tokenizer-numeric-character-reference-v1-tests)
  add_test(
    NAME html-tokenizer-numeric-character-reference-v1-tests
    COMMAND zevryon-html-tokenizer-numeric-character-reference-v1-tests)

  add_executable(
    zevryon-html-tokenizer-named-character-reference-v1-tests
    tests/html_tokenizer_named_character_reference_v1_tests.cpp)
  target_link_libraries(
    zevryon-html-tokenizer-named-character-reference-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-html-tokenizer-named-character-reference-v1-tests)
  add_test(
    NAME html-tokenizer-named-character-reference-v1-tests
    COMMAND zevryon-html-tokenizer-named-character-reference-v1-tests)
endif()
'''
Path("cmake/html_tokenizer_character_reference_v1.cmake").write_text(CMAKE, encoding="utf-8")

TEST = r'''#include "html_tokenizer_character_reference_v1.hpp"
#include "html_tokenizer_data_tags_v1.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using zevryon::massivedoc::HtmlTokenizerCharacterReferenceV1Context;
using zevryon::massivedoc::HtmlTokenizerCharacterReferenceV1Result;
using zevryon::massivedoc::HtmlTokenizerCharacterReferenceV1Stats;
using zevryon::massivedoc::HtmlTokenizerDataTagsV1Config;
using zevryon::massivedoc::HtmlTokenizerDataTagsV1Stats;
using zevryon::massivedoc::HtmlTokenizerV1ParseError;
using zevryon::massivedoc::HtmlTokenizerV1Sink;
using zevryon::massivedoc::HtmlTokenizerV1Token;
using zevryon::massivedoc::HtmlTokenizerV1TokenKind;
using zevryon::massivedoc::consume_html_character_reference_v1;
using zevryon::massivedoc::tokenize_html_data_tags_v1;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: named character-reference v1: " << message << '\n';
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

bool run_reference(
    std::string_view input,
    HtmlTokenizerCharacterReferenceV1Context context,
    CollectingSink* sink,
    HtmlTokenizerCharacterReferenceV1Stats* stats,
    HtmlTokenizerCharacterReferenceV1Result* result,
    std::string* error) {
    return consume_html_character_reference_v1(input, 0U, context, sink, stats, result, error);
}

bool test_exact_longest_and_maximum_name() {
    struct Case { std::string input; std::string output; };
    const Case cases[] = {
        {"&amp;", "&"},
        {"&notin;", std::string("\xE2\x88\x89", 3U)},
        {"&CounterClockwiseContourIntegral;", std::string("\xE2\x88\xB3", 3U)},
    };
    for (const Case& item : cases) {
        CollectingSink sink;
        HtmlTokenizerCharacterReferenceV1Stats stats;
        HtmlTokenizerCharacterReferenceV1Result result;
        std::string error;
        if (!require(run_reference(item.input, HtmlTokenizerCharacterReferenceV1Context::Data,
                                   &sink, &stats, &result, &error), error) ||
            !require(result.replacement_utf8 == item.output, "exact/longest replacement") ||
            !require(result.next_offset == item.input.size(), "exact/longest consumed length") ||
            !require(sink.errors.empty() && stats.parse_errors_emitted == 0U,
                     "exact/longest diagnostics")) {
            return false;
        }
    }
    return true;
}

bool test_two_scalar_replacement() {
    CollectingSink sink;
    HtmlTokenizerCharacterReferenceV1Stats stats;
    HtmlTokenizerCharacterReferenceV1Result result;
    std::string error;
    const std::string expected("\xE2\x89\x82\xCC\xB8", 5U);
    return require(run_reference("&NotEqualTilde;", HtmlTokenizerCharacterReferenceV1Context::Data,
                                 &sink, &stats, &result, &error), error) &&
        require(result.replacement_utf8 == expected, "two-scalar UTF-8 replacement") &&
        require(result.next_offset == 15U, "two-scalar consumed length") &&
        require(sink.errors.empty(), "two-scalar diagnostics");
}

bool test_legacy_data_longest_match() {
    const std::string not_sign("\xC2\xAC", 2U);
    for (const std::string& input : {std::string("&notit"), std::string("&notin")}) {
        CollectingSink sink;
        HtmlTokenizerCharacterReferenceV1Stats stats;
        HtmlTokenizerCharacterReferenceV1Result result;
        std::string error;
        if (!require(run_reference(input, HtmlTokenizerCharacterReferenceV1Context::Data,
                                   &sink, &stats, &result, &error), error) ||
            !require(result.replacement_utf8 == not_sign && result.next_offset == 4U,
                     "legacy longest-match result") ||
            !require(sink.errors.size() == 1U &&
                     sink.errors[0].code == "missing-semicolon-after-character-reference" &&
                     sink.errors[0].column == 5U,
                     "legacy missing-semicolon diagnostic") ||
            !require(stats.parse_errors_emitted == 1U, "legacy diagnostic stats")) {
            return false;
        }
    }
    return true;
}

bool test_attribute_legacy_veto_and_acceptance() {
    for (const std::string& input : {std::string("&notx"), std::string("&not1"), std::string("&noti")}) {
        CollectingSink sink;
        HtmlTokenizerCharacterReferenceV1Stats stats;
        HtmlTokenizerCharacterReferenceV1Result result;
        std::string error;
        if (!require(run_reference(input, HtmlTokenizerCharacterReferenceV1Context::Attribute,
                                   &sink, &stats, &result, &error), error) ||
            !require(result.replacement_utf8 == "&not" && result.next_offset == 4U,
                     "attribute legacy veto literal spelling") ||
            !require(sink.errors.empty() && stats.parse_errors_emitted == 0U,
                     "attribute legacy veto diagnostics")) {
            return false;
        }
    }

    CollectingSink sink;
    HtmlTokenizerCharacterReferenceV1Stats stats;
    HtmlTokenizerCharacterReferenceV1Result result;
    std::string error;
    const std::string copyright_sign("\xC2\xA9", 2U);
    return require(run_reference("&COPY", HtmlTokenizerCharacterReferenceV1Context::Attribute,
                                 &sink, &stats, &result, &error), error) &&
        require(result.replacement_utf8 == copyright_sign && result.next_offset == 5U,
                "legacy attribute accepted replacement") &&
        require(sink.errors.size() == 1U &&
                sink.errors[0].code == "missing-semicolon-after-character-reference" &&
                sink.errors[0].column == 6U,
                "legacy attribute missing-semicolon diagnostic");
}

bool test_ambiguous_ampersand_no_match() {
    struct Case { std::string input; std::string output; std::size_t next; std::size_t errors; };
    const Case cases[] = {
        {"&f", "&f", 2U, 0U},
        {"&no", "&no", 3U, 0U},
        {"&bogus;", "&bogus", 6U, 1U},
    };
    for (const Case& item : cases) {
        CollectingSink sink;
        HtmlTokenizerCharacterReferenceV1Stats stats;
        HtmlTokenizerCharacterReferenceV1Result result;
        std::string error;
        if (!require(run_reference(item.input, HtmlTokenizerCharacterReferenceV1Context::Data,
                                   &sink, &stats, &result, &error), error) ||
            !require(result.replacement_utf8 == item.output && result.next_offset == item.next,
                     "ambiguous ampersand literal result") ||
            !require(sink.errors.size() == item.errors, "ambiguous ampersand error count")) {
            return false;
        }
        if (item.errors == 1U &&
            !require(sink.errors[0].code == "unknown-named-character-reference" &&
                     sink.errors[0].column == 7U,
                     "unknown named-reference diagnostic")) {
            return false;
        }
    }
    return true;
}

bool test_data_and_attribute_integration() {
    const std::string not_sign("\xC2\xAC", 2U);
    const std::string not_in("\xE2\x88\x89", 3U);
    {
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Stats stats;
        std::string error;
        const std::string input = "I'm &not;it / I'm &notin; / I'm &notit / I'm &notin";
        const std::string expected = std::string("I'm ") + not_sign + "it / I'm " + not_in +
            " / I'm " + not_sign + "it / I'm " + not_sign + "in";
        if (!require(tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error), error) ||
            !require(sink.tokens.size() == 1U &&
                     sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                     sink.tokens[0].data == expected,
                     "Data named-reference integration") ||
            !require(sink.errors.size() == 2U && stats.parse_errors_emitted == 2U,
                     "Data legacy diagnostics")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Stats stats;
        std::string error;
        const std::string input = "<h a='&notx' b='&not1' c='&noti' d='&COPY'>";
        const std::string copyright_sign("\xC2\xA9", 2U);
        if (!require(tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error), error) ||
            !require(sink.tokens.size() == 1U && sink.tokens[0].attributes.size() == 4U,
                     "attribute integration token shape") ||
            !require(sink.tokens[0].attributes[0].value == "&notx" &&
                     sink.tokens[0].attributes[1].value == "&not1" &&
                     sink.tokens[0].attributes[2].value == "&noti" &&
                     sink.tokens[0].attributes[3].value == copyright_sign,
                     "attribute legacy values") ||
            !require(sink.errors.size() == 1U &&
                     sink.errors[0].code == "missing-semicolon-after-character-reference",
                     "attribute integration diagnostic")) {
            return false;
        }
    }
    return true;
}

bool test_generated_replacement_respects_cap() {
    CollectingSink sink;
    HtmlTokenizerDataTagsV1Stats stats;
    HtmlTokenizerDataTagsV1Config config;
    config.maximum_token_bytes = 4U;
    std::string error;
    return require(!tokenize_html_data_tags_v1("&NotEqualTilde;", config, &sink, &stats, &error),
                   "five-byte named replacement must exceed four-byte cap") &&
        require(error.find("coalesced character token exceeds bounded byte limit") != std::string::npos,
                "named replacement cap failure explicit") &&
        require(sink.tokens.empty(), "named replacement cap publishes no Character token");
}

} // namespace

int main() {
    return test_exact_longest_and_maximum_name() &&
            test_two_scalar_replacement() &&
            test_legacy_data_longest_match() &&
            test_attribute_legacy_veto_and_acceptance() &&
            test_ambiguous_ampersand_no_match() &&
            test_data_and_attribute_integration() &&
            test_generated_replacement_respects_cap()
        ? 0
        : 1;
}
'''
Path("tests/html_tokenizer_named_character_reference_v1_tests.cpp").write_text(TEST, encoding="utf-8")

DOC = '''# Z7 bounded HTML named character references v1

## Scope

This slice consumes the separately pinned WHATWG 2,231-entry named-character-reference table through the existing shared `consume_html_character_reference_v1()` production boundary. Numeric-reference behavior remains on the same component and is regression-tested unchanged.

Named matching is bounded by the generated 32-byte maximum entity spelling. The implementation searches candidate lengths from longest to shortest and uses binary search over the sorted generated table, so WHATWG maximum-length matching is explicit rather than approximated by a small hand-written entity list.

## Admitted behavior

The shared component now admits:

- exact semicolon-terminated named references;
- one- and two-Unicode-scalar UTF-8 replacement;
- legacy names without a semicolon with `missing-semicolon-after-character-reference`;
- attribute-context historical veto when a semicolonless match is followed by `=` or ASCII alphanumeric input;
- ambiguous-ampersand literal recovery when no table entry matches;
- `unknown-named-character-reference` when an unmatched ASCII-alphanumeric name reaches `;`.

Data and attribute callers retain their existing byte caps. Generated replacement bytes are appended under those caps before token publication.

## External test1 boundary

This production slice covers the semantics needed by ten of the eleven executions still excluded from the frozen 58/11 `tokenizer/test1.test` authority. `Non-ASCII character reference name` remains outside the current raw-input preprocessing/location authority because its input contains a non-ASCII source character.

Production capability does not itself rewrite the external-runner denominator. A separate authority-only change must measure and promote the ten named-reference executions; its expected honest target is 68 passed / 0 failed / 1 unsupported.

## Nonclaims

This slice does not admit raw non-ASCII input preprocessing, U+0000 replacement, CDATA, full WHATWG tokenizer conformance, `html_tokenizer_conformance`, tree-builder conformance, or Z7 completion. Z7 remains `planned`.
'''
Path("docs/Z7_HTML_TOKENIZER_NAMED_CHARACTER_REFERENCE_V1.md").write_text(DOC, encoding="utf-8")

replace_once(
    "docs/Z7_HTML_TOKENIZER_DATA_TAGS_V1.md",
    "- decimal and hexadecimal numeric character references in Data and attribute values.\n",
    "- decimal and hexadecimal numeric character references in Data and attribute values;\n- the complete pinned WHATWG named-character-reference table in Data and attribute values.\n",
)
replace_once(
    "docs/Z7_HTML_TOKENIZER_DATA_TAGS_V1.md",
    "- complete named character references and their longest-match/ambiguous-ampersand rules;\n",
    "",
)
replace_once(
    "docs/Z7_HTML_TOKENIZER_DATA_TAGS_V1.md",
    "The dedicated `html-tokenizer-numeric-character-reference-v1-tests` adds authority for literal ampersand fallback, decimal/hex numeric decoding, exact numeric recovery errors, C1/noncharacter/scalar validation, decoded UTF-8 output, Data coalescing, quoted/unquoted attribute integration, named-reference fail-closed behavior and replacement byte caps.\n",
    "The dedicated `html-tokenizer-numeric-character-reference-v1-tests` retains authority for literal ampersand fallback, decimal/hex numeric decoding, exact numeric recovery errors, C1/noncharacter/scalar validation, decoded UTF-8 output, Data coalescing, quoted/unquoted attribute integration and replacement byte caps. `html-tokenizer-named-character-reference-v1-tests` adds full pinned-table longest-match, two-scalar output, legacy-semicolon recovery, attribute veto, ambiguous-ampersand and named replacement-cap authority.\n",
)
replace_once(
    "docs/Z7_HTML_TOKENIZER_DATA_TAGS_V1.md",
    "The admitted `test1.test` runner remains a separate authority surface, and unsupported cases continue to be counted explicitly rather than converted to passes. Complete named character references, broader recovery, preprocessing, CDATA and tree-builder conformance remain open.\n",
    "The admitted `test1.test` runner remains a separate authority surface, and unsupported cases continue to be counted explicitly rather than converted to passes. Raw-input preprocessing/non-ASCII authority, broader recovery, CDATA and tree-builder conformance remain open.\n",
)

replace_once(
    "docs/Z7_HTML_TOKENIZER_NUMERIC_CHARACTER_REFERENCE_V1.md",
    "Complete named character references are explicitly outside this slice. An ASCII alphanumeric named candidate therefore fails closed rather than approximating the named-reference table.\n",
    "Complete named character references were explicitly outside this numeric slice at admission time. The later named-reference slice extends the same shared component using the separately pinned complete table; this document retains the numeric slice's independent scope.\n",
)
