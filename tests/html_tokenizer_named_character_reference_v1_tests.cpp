#include "html_tokenizer_character_reference_v1.hpp"
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
