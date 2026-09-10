#include "html_tokenizer_data_tags_v1.hpp"

#include "html_tokenizer_character_reference_v1.hpp"
#include "html_tokenizer_utf8_v1.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zevryon::massivedoc {
namespace {

constexpr std::size_t kMaximumConfiguredInputBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumConfiguredTokenBytes = 1024U * 1024U;
constexpr std::size_t kMaximumConfiguredAttributes = 4096U;

bool fail_data_tokenizer(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool ascii_byte(char value) noexcept {
    return static_cast<unsigned char>(value) < 0x80U;
}

bool ascii_alpha(char value) noexcept {
    return (value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z');
}

bool ascii_digit(char value) noexcept {
    return value >= '0' && value <= '9';
}

bool ascii_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' ||
        value == '\r' || value == '\f';
}

char ascii_lower(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

bool tag_name_character(char value) noexcept {
    return ascii_alpha(value) || ascii_digit(value) || value == '-';
}

bool attribute_name_character(char value) noexcept {
    return ascii_alpha(value) || ascii_digit(value) || value == '-' ||
        value == '_' || value == ':';
}

bool increment_counter(
    std::uint64_t* value,
    std::string* error,
    std::string_view label) {
    if (*value == std::numeric_limits<std::uint64_t>::max()) {
        return fail_data_tokenizer(error, std::string(label) + " counter overflow");
    }
    ++*value;
    return true;
}

bool add_counter(
    std::uint64_t* value,
    std::uint64_t increment,
    std::string* error,
    std::string_view label) {
    if (*value > std::numeric_limits<std::uint64_t>::max() - increment) {
        return fail_data_tokenizer(error, std::string(label) + " counter overflow");
    }
    *value += increment;
    return true;
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
    for (std::size_t index = 0U; index < limit;) {
        if (input[index] == '\n') {
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
    return result;
}

bool validate_config(
    const HtmlTokenizerDataTagsV1Config& config,
    std::string* error) {
    if (config.maximum_input_bytes == 0U ||
        config.maximum_input_bytes > kMaximumConfiguredInputBytes) {
        return fail_data_tokenizer(
            error,
            "HTML Data-tag tokenizer input bound is outside supported range");
    }
    if (config.maximum_token_bytes == 0U ||
        config.maximum_token_bytes > kMaximumConfiguredTokenBytes) {
        return fail_data_tokenizer(
            error,
            "HTML Data-tag tokenizer token bound is outside supported range");
    }
    if (config.maximum_attributes == 0U ||
        config.maximum_attributes > kMaximumConfiguredAttributes) {
        return fail_data_tokenizer(
            error,
            "HTML Data-tag tokenizer attribute bound is outside supported range");
    }
    return true;
}

class DataTagTokenizer final {
public:
    DataTagTokenizer(
        std::string_view input,
        HtmlTokenizerDataTagsV1Config config,
        HtmlTokenizerV1Sink* sink,
        HtmlTokenizerDataTagsV1Stats* stats,
        std::string* error)
        : input_(input),
          config_(config),
          sink_(sink),
          stats_(stats),
          error_(error) {
        character_buffer_.reserve(
            std::min<std::size_t>(config_.maximum_token_bytes, 4096U));
    }

    bool run() {
        std::size_t cursor = 0U;
        while (cursor < input_.size()) {
            const char character = input_[cursor];
            if (character == '\0') {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");
            }
            if (!ascii_byte(character)) {
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
                if (!consume_character_reference(
                        &cursor,
                        HtmlTokenizerCharacterReferenceV1Context::Data,
                        &character_buffer_,
                        "HTML Data-tag tokenizer coalesced character token")) {
                    return false;
                }
                continue;
            }
            if (character == '<') {
                if (!consume_tag(&cursor)) {
                    return false;
                }
                continue;
            }
            if (!append_character(character)) {
                return false;
            }
            ++cursor;
        }
        return flush_character();
    }

private:
    bool append_character(char character) {
        if (!ascii_byte(character)) {
            return fail_data_tokenizer(
                error_,
                "HTML Data-tag tokenizer non-ASCII preprocessing/location authority is not implemented");
        }
        if (character_buffer_.size() >= config_.maximum_token_bytes) {
            return fail_data_tokenizer(
                error_,
                "HTML Data-tag tokenizer coalesced character token exceeds bounded byte limit");
        }
        character_buffer_.push_back(character);
        return true;
    }

    bool append_bounded_bytes(
        std::string* value,
        std::string_view bytes,
        std::string_view label) {
        if (value->size() > config_.maximum_token_bytes ||
            bytes.size() > config_.maximum_token_bytes - value->size()) {
            return fail_data_tokenizer(
                error_,
                std::string(label) + " exceeds bounded byte limit");
        }
        value->append(bytes.data(), bytes.size());
        return true;
    }

    bool consume_character_reference(
        std::size_t* cursor,
        HtmlTokenizerCharacterReferenceV1Context context,
        std::string* destination,
        std::string_view destination_label) {
        if (cursor == nullptr || destination == nullptr || *cursor >= input_.size()) {
            return fail_data_tokenizer(
                error_,
                "HTML Data-tag tokenizer character-reference dispatch invariant failed");
        }

        const std::size_t reference_start = *cursor;
        HtmlTokenizerCharacterReferenceV1Stats reference_stats{};
        HtmlTokenizerCharacterReferenceV1Result reference_result{};
        const bool success = consume_html_character_reference_v1(
            input_,
            reference_start,
            context,
            sink_,
            &reference_stats,
            &reference_result,
            error_);

        if (!add_counter(
                &stats_->parse_errors_emitted,
                reference_stats.parse_errors_emitted,
                error_,
                "HTML Data-tag tokenizer parse-error")) {
            return false;
        }
        if (!success) {
            return false;
        }
        if (reference_result.next_offset <= reference_start ||
            reference_result.next_offset > input_.size() ||
            reference_result.replacement_utf8.empty()) {
            return fail_data_tokenizer(
                error_,
                "HTML Data-tag tokenizer character-reference result invariant failed");
        }
        if (!append_bounded_bytes(
                destination,
                reference_result.replacement_utf8,
                destination_label)) {
            return false;
        }
        *cursor = reference_result.next_offset;
        return true;
    }

    bool flush_character() {
        if (character_buffer_.empty()) {
            return true;
        }
        HtmlTokenizerV1Token token;
        token.kind = HtmlTokenizerV1TokenKind::Character;
        token.data = character_buffer_;
        if (!sink_->on_token(token, error_)) {
            if (error_->empty()) {
                *error_ = "HTML Data-tag tokenizer sink rejected character token";
            }
            return false;
        }
        if (!increment_counter(
                &stats_->tokens_emitted,
                error_,
                "HTML Data-tag tokenizer token") ||
            !increment_counter(
                &stats_->character_tokens_emitted,
                error_,
                "HTML Data-tag tokenizer character-token") ||
            !add_counter(
                &stats_->character_bytes_emitted,
                static_cast<std::uint64_t>(character_buffer_.size()),
                error_,
                "HTML Data-tag tokenizer character-byte")) {
            return false;
        }
        character_buffer_.clear();
        return true;
    }

    bool emit_parse_error(std::size_t offset, std::string code) {
        HtmlTokenizerV1ParseError parse_error =
            position_error(input_, offset, std::move(code));
        if (!sink_->on_parse_error(parse_error, error_)) {
            if (error_->empty()) {
                *error_ = "HTML Data-tag tokenizer sink rejected parse error";
            }
            return false;
        }
        return increment_counter(
            &stats_->parse_errors_emitted,
            error_,
            "HTML Data-tag tokenizer parse-error");
    }

    bool append_name_character(std::string* value, char character) {
        if (!ascii_byte(character)) {
            return fail_data_tokenizer(
                error_,
                "HTML Data-tag tokenizer non-ASCII name authority is not implemented");
        }
        if (value->size() >= config_.maximum_token_bytes) {
            return fail_data_tokenizer(
                error_,
                "HTML Data-tag tokenizer name exceeds bounded token byte limit");
        }
        value->push_back(ascii_lower(character));
        return true;
    }

    bool append_value_character(std::string* value, char character) {
        if (!ascii_byte(character)) {
            return fail_data_tokenizer(
                error_,
                "HTML Data-tag tokenizer non-ASCII attribute-value authority is not implemented");
        }
        if (value->size() >= config_.maximum_token_bytes) {
            return fail_data_tokenizer(
                error_,
                "HTML Data-tag tokenizer attribute value exceeds bounded token byte limit");
        }
        value->push_back(character);
        return true;
    }

    bool token_budget_accepts(
        std::size_t current,
        std::size_t additional) const noexcept {
        return current <= config_.maximum_token_bytes &&
            additional <= config_.maximum_token_bytes - current;
    }

    bool parse_attribute_value(
        std::size_t* cursor,
        std::string* value) {
        if (*cursor >= input_.size()) {
            return fail_data_tokenizer(
                error_,
                "HTML Data-tag tokenizer EOF after attribute equals is outside admitted recovery");
        }

        const char opening = input_[*cursor];
        if (opening == '\'' || opening == '"') {
            ++*cursor;
            while (*cursor < input_.size() && input_[*cursor] != opening) {
                const char character = input_[*cursor];
                if (character == '\0') {
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");
                }
                if (!ascii_byte(character)) {
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer non-ASCII attribute-value authority is not implemented");
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
                    return false;
                }
                ++*cursor;
            }
            if (*cursor == input_.size()) {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer EOF in quoted attribute value is outside admitted recovery");
            }
            ++*cursor;
            return true;
        }

        while (*cursor < input_.size()) {
            const char character = input_[*cursor];
            if (ascii_space(character) || character == '>') {
                break;
            }
            if (character == '/' &&
                *cursor + 1U < input_.size() &&
                input_[*cursor + 1U] == '>') {
                break;
            }
            if (character == '\0') {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");
            }
            if (!ascii_byte(character)) {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer non-ASCII attribute-value authority is not implemented");
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
            if (character == '"' || character == '\'' || character == '<' ||
                character == '=' || character == '`') {
                if (!emit_parse_error(
                        *cursor,
                        "unexpected-character-in-unquoted-attribute-value")) {
                    return false;
                }
            }
            if (!append_value_character(value, character)) {
                return false;
            }
            ++*cursor;
        }
        return true;
    }

    bool parse_attribute(
        std::size_t* cursor,
        std::vector<HtmlTokenizerV1Attribute>* attributes,
        std::size_t* token_bytes) {
        const std::size_t name_begin = *cursor;
        HtmlTokenizerV1Attribute attribute;
        while (*cursor < input_.size() &&
               attribute_name_character(input_[*cursor])) {
            if (!append_name_character(&attribute.name, input_[*cursor])) {
                return false;
            }
            ++*cursor;
        }
        if (*cursor == name_begin) {
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
            if (!parse_attribute_value(cursor, &attribute.value)) {
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

    bool emit_tag(
        std::string name,
        std::vector<HtmlTokenizerV1Attribute> attributes,
        bool end_tag,
        bool self_closing) {
        if (!flush_character()) {
            return false;
        }

        HtmlTokenizerV1Token token;
        token.kind = end_tag
            ? HtmlTokenizerV1TokenKind::EndTag
            : HtmlTokenizerV1TokenKind::StartTag;
        token.name = std::move(name);
        if (!end_tag) {
            token.attributes = std::move(attributes);
            token.self_closing = self_closing;
        }

        const std::uint64_t attribute_count =
            static_cast<std::uint64_t>(token.attributes.size());
        if (!sink_->on_token(token, error_)) {
            if (error_->empty()) {
                *error_ = end_tag
                    ? "HTML Data-tag tokenizer sink rejected end-tag token"
                    : "HTML Data-tag tokenizer sink rejected start-tag token";
            }
            return false;
        }
        if (!increment_counter(
                &stats_->tokens_emitted,
                error_,
                "HTML Data-tag tokenizer token")) {
            return false;
        }
        if (end_tag) {
            return increment_counter(
                &stats_->end_tags_emitted,
                error_,
                "HTML Data-tag tokenizer end-tag");
        }
        return increment_counter(
                   &stats_->start_tags_emitted,
                   error_,
                   "HTML Data-tag tokenizer start-tag") &&
            add_counter(
                &stats_->attributes_emitted,
                attribute_count,
                error_,
                "HTML Data-tag tokenizer attribute");
    }

    bool consume_tag(std::size_t* cursor) {
        const std::size_t tag_open = *cursor;
        std::size_t probe = tag_open + 1U;
        if (probe >= input_.size()) {
            return fail_data_tokenizer(
                error_,
                "HTML Data-tag tokenizer EOF after tag-open is outside admitted recovery");
        }

        if (input_[probe] == '!') {
            return fail_data_tokenizer(
                error_,
                "HTML Data-tag tokenizer comments/DOCTYPE/markup declarations are outside admitted v1 subset");
        }
        if (input_[probe] == '?') {
            return fail_data_tokenizer(
                error_,
                "HTML Data-tag tokenizer bogus-comment recovery is outside admitted v1 subset");
        }

        bool end_tag = false;
        if (input_[probe] == '/') {
            end_tag = true;
            ++probe;
            if (probe >= input_.size()) {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer EOF after end-tag open is outside admitted recovery");
            }
            if (input_[probe] == '>') {
                if (!emit_parse_error(probe, "missing-end-tag-name")) {
                    return false;
                }
                *cursor = probe + 1U;
                return true;
            }
        }

        if (!ascii_alpha(input_[probe])) {
            if (input_[probe] == '\0') {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");
            }
            if (!ascii_byte(input_[probe])) {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer non-ASCII preprocessing/location authority is not implemented");
            }
            if (end_tag) {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer bogus-comment end-tag recovery is outside admitted v1 subset");
            }
            if (!emit_parse_error(
                    probe,
                    "invalid-first-character-of-tag-name") ||
                !append_character('<')) {
                return false;
            }
            // WHATWG tag-open "anything else" emits '<' then reconsumes the
            // current byte in Data state. Leave probe unconsumed for run().
            *cursor = probe;
            return true;
        }

        std::string name;
        while (probe < input_.size() && tag_name_character(input_[probe])) {
            if (!append_name_character(&name, input_[probe])) {
                return false;
            }
            ++probe;
        }

        std::size_t token_bytes = name.size();
        std::vector<HtmlTokenizerV1Attribute> attributes;
        bool self_closing = false;
        bool parsed_attribute = false;

        while (true) {
            bool had_whitespace = false;
            while (probe < input_.size() && ascii_space(input_[probe])) {
                had_whitespace = true;
                ++probe;
            }
            if (probe >= input_.size()) {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer EOF in tag is outside admitted recovery");
            }

            if (input_[probe] == '>') {
                const std::size_t close_offset = probe;
                ++probe;
                if (end_tag && !attributes.empty()) {
                    if (!emit_parse_error(
                            close_offset,
                            "end-tag-with-attributes")) {
                        return false;
                    }
                    attributes.clear();
                }
                if (end_tag && self_closing) {
                    if (!emit_parse_error(
                            close_offset,
                            "end-tag-with-trailing-solidus")) {
                        return false;
                    }
                }
                if (!emit_tag(
                        std::move(name),
                        std::move(attributes),
                        end_tag,
                        self_closing)) {
                    return false;
                }
                *cursor = probe;
                return true;
            }

            if (input_[probe] == '/' &&
                probe + 1U < input_.size() &&
                input_[probe + 1U] == '>') {
                self_closing = true;
                const std::size_t close_offset = probe + 1U;
                probe += 2U;
                if (end_tag && !attributes.empty()) {
                    if (!emit_parse_error(
                            close_offset,
                            "end-tag-with-attributes")) {
                        return false;
                    }
                    attributes.clear();
                }
                if (end_tag &&
                    !emit_parse_error(
                        close_offset,
                        "end-tag-with-trailing-solidus")) {
                    return false;
                }
                if (!emit_tag(
                        std::move(name),
                        std::move(attributes),
                        end_tag,
                        self_closing)) {
                    return false;
                }
                *cursor = probe;
                return true;
            }

            if (!had_whitespace) {
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
                return false;
            }
            parsed_attribute = true;
        }
    }

    std::string_view input_;
    HtmlTokenizerDataTagsV1Config config_{};
    HtmlTokenizerV1Sink* sink_{nullptr};
    HtmlTokenizerDataTagsV1Stats* stats_{nullptr};
    std::string* error_{nullptr};
    std::string character_buffer_;
};

} // namespace

bool tokenize_html_data_tags_v1(
    std::string_view input,
    HtmlTokenizerDataTagsV1Config config,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerDataTagsV1Stats* stats,
    std::string* error) {
    if (error == nullptr || sink == nullptr) {
        return false;
    }
    error->clear();

    HtmlTokenizerDataTagsV1Stats local_stats{};
    local_stats.input_bytes = static_cast<std::uint64_t>(input.size());
    if (stats != nullptr) {
        *stats = local_stats;
    }
    if (!validate_config(config, error)) {
        return false;
    }
    if (input.size() > config.maximum_input_bytes) {
        return fail_data_tokenizer(
            error,
            "HTML Data-tag tokenizer input exceeds bounded byte limit");
    }

    bool success = false;
    try {
        DataTagTokenizer tokenizer(
            input,
            config,
            sink,
            &local_stats,
            error);
        success = tokenizer.run();
    } catch (const std::bad_alloc&) {
        success = fail_data_tokenizer(
            error,
            "HTML Data-tag tokenizer allocation failed within bounded v1 slice");
    }

    if (stats != nullptr) {
        *stats = local_stats;
    }
    return success;
}

} // namespace zevryon::massivedoc
