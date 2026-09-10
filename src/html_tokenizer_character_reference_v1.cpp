#include "html_tokenizer_character_reference_v1.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace zevryon::massivedoc {
namespace {

bool fail_reference(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool ascii_alpha(char value) noexcept {
    return (value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z');
}

bool ascii_digit(char value) noexcept {
    return value >= '0' && value <= '9';
}

bool ascii_alphanumeric(char value) noexcept {
    return ascii_alpha(value) || ascii_digit(value);
}

bool ascii_hex_digit(char value) noexcept {
    return ascii_digit(value) ||
        (value >= 'a' && value <= 'f') ||
        (value >= 'A' && value <= 'F');
}

std::uint32_t digit_value(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint32_t>(value - '0');
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<std::uint32_t>(value - 'A' + 10);
    }
    return static_cast<std::uint32_t>(value - 'a' + 10);
}

HtmlTokenizerV1ParseError position_error(
    std::string_view input,
    std::size_t offset,
    std::string code) {
    HtmlTokenizerV1ParseError result;
    result.code = std::move(code);
    result.line = 1U;
    result.column = 1U;
    const std::size_t limit = std::min(offset, input.size());
    for (std::size_t index = 0U; index < limit; ++index) {
        if (input[index] == '\n') {
            ++result.line;
            result.column = 1U;
        } else {
            ++result.column;
        }
    }
    return result;
}

bool emit_parse_error(
    std::string_view input,
    std::size_t offset,
    std::string code,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerCharacterReferenceV1Stats* stats,
    std::string* error) {
    HtmlTokenizerV1ParseError parse_error =
        position_error(input, offset, std::move(code));
    if (!sink->on_parse_error(parse_error, error)) {
        if (error->empty()) {
            *error = "HTML character-reference sink rejected parse error";
        }
        return false;
    }
    if (stats->parse_errors_emitted ==
        std::numeric_limits<std::uint64_t>::max()) {
        return fail_reference(
            error,
            "HTML character-reference parse-error counter overflow");
    }
    ++stats->parse_errors_emitted;
    return true;
}

bool is_noncharacter(std::uint32_t codepoint) noexcept {
    if (codepoint >= 0xFDD0U && codepoint <= 0xFDEFU) {
        return true;
    }
    return codepoint <= 0x10FFFFU &&
        (codepoint & 0xFFFEU) == 0xFFFEU;
}

bool is_disallowed_control(std::uint32_t codepoint) noexcept {
    if (codepoint == 0x0DU) {
        return true;
    }
    if (codepoint <= 0x1FU) {
        return codepoint != 0x09U &&
            codepoint != 0x0AU &&
            codepoint != 0x0CU;
    }
    return codepoint >= 0x7FU && codepoint <= 0x9FU;
}

std::uint32_t remap_c1_control(std::uint32_t codepoint) noexcept {
    switch (codepoint) {
    case 0x80U: return 0x20ACU;
    case 0x82U: return 0x201AU;
    case 0x83U: return 0x0192U;
    case 0x84U: return 0x201EU;
    case 0x85U: return 0x2026U;
    case 0x86U: return 0x2020U;
    case 0x87U: return 0x2021U;
    case 0x88U: return 0x02C6U;
    case 0x89U: return 0x2030U;
    case 0x8AU: return 0x0160U;
    case 0x8BU: return 0x2039U;
    case 0x8CU: return 0x0152U;
    case 0x8EU: return 0x017DU;
    case 0x91U: return 0x2018U;
    case 0x92U: return 0x2019U;
    case 0x93U: return 0x201CU;
    case 0x94U: return 0x201DU;
    case 0x95U: return 0x2022U;
    case 0x96U: return 0x2013U;
    case 0x97U: return 0x2014U;
    case 0x98U: return 0x02DCU;
    case 0x99U: return 0x2122U;
    case 0x9AU: return 0x0161U;
    case 0x9BU: return 0x203AU;
    case 0x9CU: return 0x0153U;
    case 0x9EU: return 0x017EU;
    case 0x9FU: return 0x0178U;
    default: return codepoint;
    }
}

bool append_utf8(
    std::uint32_t codepoint,
    std::string* output,
    std::string* error) {
    if (codepoint <= 0x7FU) {
        output->push_back(static_cast<char>(codepoint));
        return true;
    }
    if (codepoint <= 0x7FFU) {
        output->push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
        output->push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        return true;
    }
    if (codepoint <= 0xFFFFU) {
        if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
            return fail_reference(
                error,
                "HTML character-reference UTF-8 encoder received surrogate");
        }
        output->push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
        output->push_back(
            static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        output->push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        return true;
    }
    if (codepoint <= 0x10FFFFU) {
        output->push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
        output->push_back(
            static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
        output->push_back(
            static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        output->push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        return true;
    }
    return fail_reference(
        error,
        "HTML character-reference UTF-8 encoder received out-of-range scalar");
}

bool validate_context(
    HtmlTokenizerCharacterReferenceV1Context context,
    std::string* error) {
    switch (context) {
    case HtmlTokenizerCharacterReferenceV1Context::Data:
    case HtmlTokenizerCharacterReferenceV1Context::Attribute:
        return true;
    }
    return fail_reference(error, "HTML character-reference context is invalid");
}

bool consume_impl(
    std::string_view input,
    std::size_t ampersand_offset,
    HtmlTokenizerCharacterReferenceV1Context context,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerCharacterReferenceV1Stats* stats,
    HtmlTokenizerCharacterReferenceV1Result* result,
    std::string* error) {
    if (!validate_context(context, error)) {
        return false;
    }
    if (ampersand_offset >= input.size() ||
        input[ampersand_offset] != '&') {
        return fail_reference(
            error,
            "HTML character-reference invocation must begin at ampersand");
    }

    std::size_t cursor = ampersand_offset + 1U;
    if (cursor == input.size()) {
        result->replacement_utf8 = "&";
        result->next_offset = cursor;
        return true;
    }

    const char first = input[cursor];
    if (ascii_alphanumeric(first)) {
        return fail_reference(
            error,
            "HTML named character references are outside admitted numeric v1 subset");
    }
    if (first != '#') {
        result->replacement_utf8 = "&";
        result->next_offset = cursor;
        return true;
    }

    std::string temporary = "&#";
    ++cursor;
    bool hexadecimal = false;
    if (cursor < input.size() &&
        (input[cursor] == 'x' || input[cursor] == 'X')) {
        hexadecimal = true;
        temporary.push_back(input[cursor]);
        ++cursor;
    }

    const auto digit_matches = [hexadecimal](char value) noexcept {
        return hexadecimal ? ascii_hex_digit(value) : ascii_digit(value);
    };
    if (cursor == input.size() || !digit_matches(input[cursor])) {
        if (!emit_parse_error(
                input,
                cursor,
                "absence-of-digits-in-numeric-character-reference",
                sink,
                stats,
                error)) {
            return false;
        }
        result->replacement_utf8 = std::move(temporary);
        result->next_offset = cursor;
        return true;
    }

    const std::uint32_t base = hexadecimal ? 16U : 10U;
    constexpr std::uint32_t kOverflowSentinel = 0x110000U;
    std::uint32_t codepoint = 0U;
    while (cursor < input.size() && digit_matches(input[cursor])) {
        const std::uint32_t digit = digit_value(input[cursor]);
        if (codepoint >= kOverflowSentinel ||
            codepoint > (kOverflowSentinel - digit) / base) {
            codepoint = kOverflowSentinel;
        } else {
            codepoint = codepoint * base + digit;
            if (codepoint > kOverflowSentinel) {
                codepoint = kOverflowSentinel;
            }
        }
        ++cursor;
    }

    if (cursor < input.size() && input[cursor] == ';') {
        ++cursor;
    } else if (!emit_parse_error(
                   input,
                   cursor,
                   "missing-semicolon-after-character-reference",
                   sink,
                   stats,
                   error)) {
        return false;
    }

    const std::size_t end_state_offset = cursor;
    if (codepoint == 0U) {
        if (!emit_parse_error(
                input,
                end_state_offset,
                "null-character-reference",
                sink,
                stats,
                error)) {
            return false;
        }
        codepoint = 0xFFFDU;
    } else if (codepoint > 0x10FFFFU) {
        if (!emit_parse_error(
                input,
                end_state_offset,
                "character-reference-outside-unicode-range",
                sink,
                stats,
                error)) {
            return false;
        }
        codepoint = 0xFFFDU;
    } else if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
        if (!emit_parse_error(
                input,
                end_state_offset,
                "surrogate-character-reference",
                sink,
                stats,
                error)) {
            return false;
        }
        codepoint = 0xFFFDU;
    } else {
        if (is_noncharacter(codepoint) &&
            !emit_parse_error(
                input,
                end_state_offset,
                "noncharacter-character-reference",
                sink,
                stats,
                error)) {
            return false;
        }
        if (is_disallowed_control(codepoint)) {
            if (!emit_parse_error(
                    input,
                    end_state_offset,
                    "control-character-reference",
                    sink,
                    stats,
                    error)) {
                return false;
            }
            codepoint = remap_c1_control(codepoint);
        }
    }

    result->replacement_utf8.clear();
    if (!append_utf8(codepoint, &result->replacement_utf8, error)) {
        return false;
    }
    result->next_offset = cursor;
    return true;
}

} // namespace

bool consume_html_character_reference_v1(
    std::string_view input,
    std::size_t ampersand_offset,
    HtmlTokenizerCharacterReferenceV1Context context,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerCharacterReferenceV1Stats* stats,
    HtmlTokenizerCharacterReferenceV1Result* result,
    std::string* error) {
    if (sink == nullptr || result == nullptr || error == nullptr) {
        return false;
    }
    error->clear();

    HtmlTokenizerCharacterReferenceV1Stats local_stats{};
    HtmlTokenizerCharacterReferenceV1Result local_result{};
    local_result.next_offset = ampersand_offset;
    if (stats != nullptr) {
        *stats = local_stats;
    }
    *result = local_result;

    bool success = false;
    try {
        success = consume_impl(
            input,
            ampersand_offset,
            context,
            sink,
            &local_stats,
            &local_result,
            error);
    } catch (const std::bad_alloc&) {
        success = fail_reference(
            error,
            "HTML character-reference allocation failed within bounded v1 slice");
    }

    if (stats != nullptr) {
        *stats = local_stats;
    }
    if (success) {
        *result = std::move(local_result);
    }
    return success;
}

} // namespace zevryon::massivedoc
