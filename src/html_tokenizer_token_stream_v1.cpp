#include "html_tokenizer_token_stream_v1.hpp"

#include "html_tokenizer_character_reference_v1.hpp"
#include "html_tokenizer_data_stream_v1.hpp"
#include "html_tokenizer_script_data_v1.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace zevryon::massivedoc {
namespace {

constexpr std::size_t kMaximumConfiguredInputBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumConfiguredTokenBytes = 1024U * 1024U;

enum class ActiveState : std::uint8_t {
    Plaintext,
    Rcdata,
    Rawtext,
    Data,
};

bool fail_tokenizer(std::string* error, std::string message) {
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

std::string ascii_lower_copy(std::string_view value) {
    std::string output;
    output.reserve(value.size());
    for (char character : value) {
        output.push_back(ascii_lower(character));
    }
    return output;
}

bool ascii_iequals_at(
    std::string_view input,
    std::size_t offset,
    std::string_view expected) noexcept {
    if (offset > input.size() || expected.size() > input.size() - offset) {
        return false;
    }
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        if (ascii_lower(input[offset + index]) != ascii_lower(expected[index])) {
            return false;
        }
    }
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

bool increment_counter(
    std::uint64_t* value,
    std::string* error,
    std::string_view label) {
    if (*value == std::numeric_limits<std::uint64_t>::max()) {
        return fail_tokenizer(error, std::string(label) + " counter overflow");
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
        return fail_tokenizer(error, std::string(label) + " counter overflow");
    }
    *value += increment;
    return true;
}

bool validate_config(const HtmlTokenizerV1Config& config, std::string* error) {
    if (config.maximum_input_bytes == 0U ||
        config.maximum_input_bytes > kMaximumConfiguredInputBytes) {
        return fail_tokenizer(
            error,
            "HTML tokenizer input bound is outside supported range");
    }
    if (config.maximum_token_bytes == 0U ||
        config.maximum_token_bytes > kMaximumConfiguredTokenBytes) {
        return fail_tokenizer(
            error,
            "HTML tokenizer token bound is outside supported range");
    }
    return true;
}

bool validate_last_start_tag(
    std::string_view last_start_tag,
    const HtmlTokenizerV1Config& config,
    std::string* normalized,
    std::string* error) {
    if (last_start_tag.size() > config.maximum_token_bytes) {
        return fail_tokenizer(
            error,
            "HTML tokenizer last-start-tag exceeds bounded byte limit");
    }
    normalized->clear();
    normalized->reserve(last_start_tag.size());
    for (char character : last_start_tag) {
        if (!tag_name_character(character)) {
            return fail_tokenizer(
                error,
                "HTML tokenizer last-start-tag contains unsupported byte");
        }
        normalized->push_back(ascii_lower(character));
    }
    return true;
}

class OffsetSink final : public HtmlTokenizerV1Sink {
public:
    OffsetSink(
        HtmlTokenizerV1Sink* downstream,
        std::uint64_t base_line,
        std::uint64_t base_column)
        : downstream_(downstream),
          base_line_(base_line),
          base_column_(base_column) {}

    bool on_token(const HtmlTokenizerV1Token& token, std::string* error) override {
        return downstream_->on_token(token, error);
    }

    bool on_parse_error(
        const HtmlTokenizerV1ParseError& parse_error,
        std::string* error) override {
        HtmlTokenizerV1ParseError translated = parse_error;
        if (parse_error.line == 1U) {
            translated.line = base_line_;
            translated.column = base_column_ + parse_error.column - 1U;
        } else {
            translated.line = base_line_ + parse_error.line - 1U;
            translated.column = parse_error.column;
        }
        return downstream_->on_parse_error(translated, error);
    }

private:
    HtmlTokenizerV1Sink* downstream_{nullptr};
    std::uint64_t base_line_{1U};
    std::uint64_t base_column_{1U};
};

bool account_script_stats(
    const HtmlTokenizerScriptDataV1Stats& source,
    HtmlTokenizerV1Stats* destination,
    std::string* error) {
    return add_counter(
               &destination->tokens_emitted,
               source.tokens_emitted,
               error,
               "HTML tokenizer Script-data token") &&
        add_counter(
               &destination->character_tokens_emitted,
               source.character_tokens_emitted,
               error,
               "HTML tokenizer Script-data character-token") &&
        add_counter(
               &destination->character_bytes_emitted,
               source.character_bytes_emitted,
               error,
               "HTML tokenizer Script-data character-byte") &&
        add_counter(
               &destination->end_tags_emitted,
               source.end_tags_emitted,
               error,
               "HTML tokenizer Script-data end-tag") &&
        add_counter(
               &destination->parse_errors_emitted,
               source.parse_errors_emitted,
               error,
               "HTML tokenizer Script-data parse-error");
}

bool account_data_stats(
    const HtmlTokenizerDataStreamV1Stats& source,
    HtmlTokenizerV1Stats* destination,
    std::string* error) {
    return add_counter(
               &destination->tokens_emitted,
               source.tokens_emitted,
               error,
               "HTML tokenizer Data-stream token") &&
        add_counter(
               &destination->character_tokens_emitted,
               source.character_tokens_emitted,
               error,
               "HTML tokenizer Data-stream character-token") &&
        add_counter(
               &destination->character_bytes_emitted,
               source.character_bytes_emitted,
               error,
               "HTML tokenizer Data-stream character-byte") &&
        add_counter(
               &destination->end_tags_emitted,
               source.end_tags_emitted,
               error,
               "HTML tokenizer Data-stream end-tag") &&
        add_counter(
               &destination->parse_errors_emitted,
               source.parse_errors_emitted,
               error,
               "HTML tokenizer Data-stream parse-error");
}

bool run_data_canonical(
    std::string_view input,
    HtmlTokenizerV1Config config,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerV1Stats* stats,
    std::string* error) {
    HtmlTokenizerDataStreamV1Config data_config{};
    data_config.maximum_input_bytes = config.maximum_input_bytes;
    data_config.maximum_token_bytes = config.maximum_token_bytes;
    data_config.maximum_attributes = 256U;

    HtmlTokenizerDataStreamV1Stats data_stats{};
    const bool data_success = tokenize_html_data_stream_v1(
        input,
        data_config,
        sink,
        &data_stats,
        error);
    if (!account_data_stats(data_stats, stats, error)) {
        return false;
    }
    return data_success;
}

bool run_script_data_canonical(
    std::string_view input,
    std::string_view last_start_tag,
    HtmlTokenizerV1Config config,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerV1Stats* stats,
    std::string* error) {
    HtmlTokenizerScriptDataV1Stats script_stats{};
    HtmlTokenizerScriptDataV1Result script_result{};
    const bool script_success = consume_html_script_data_v1(
        input,
        last_start_tag,
        config,
        sink,
        &script_stats,
        &script_result,
        error);
    if (!account_script_stats(script_stats, stats, error)) {
        return false;
    }
    if (!script_success) {
        return false;
    }
    if (!script_result.transitioned_to_data) {
        return true;
    }
    if (script_result.next_offset > input.size()) {
        return fail_tokenizer(
            error,
            "HTML tokenizer Script-data transition offset exceeds input");
    }

    const HtmlTokenizerV1ParseError base =
        position_error(input, script_result.next_offset, "");
    OffsetSink translated_sink(sink, base.line, base.column);

    HtmlTokenizerDataStreamV1Config data_config{};
    data_config.maximum_input_bytes = config.maximum_input_bytes;
    data_config.maximum_token_bytes = config.maximum_token_bytes;
    data_config.maximum_attributes = 256U;

    HtmlTokenizerDataStreamV1Stats data_stats{};
    const bool data_success = tokenize_html_data_stream_v1(
        input.substr(script_result.next_offset),
        data_config,
        &translated_sink,
        &data_stats,
        error);
    if (!account_data_stats(data_stats, stats, error)) {
        return false;
    }
    return data_success;
}

class Tokenizer {
public:
    Tokenizer(
        std::string_view input,
        ActiveState state,
        std::string last_start_tag,
        HtmlTokenizerV1Config config,
        HtmlTokenizerV1Sink* sink,
        HtmlTokenizerV1Stats* stats,
        std::string* error)
        : input_(input),
          state_(state),
          last_start_tag_(std::move(last_start_tag)),
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
            if (input_[cursor] == '\0') {
                return fail_tokenizer(
                    error_,
                    "HTML tokenizer input preprocessing/NUL replacement is not implemented in v1 token stream");
            }
            switch (state_) {
            case ActiveState::Plaintext:
                if (!append_character(input_[cursor])) {
                    return false;
                }
                ++cursor;
                break;
            case ActiveState::Rcdata:
                if (!consume_text_state(&cursor, true)) {
                    return false;
                }
                break;
            case ActiveState::Rawtext:
                if (!consume_text_state(&cursor, false)) {
                    return false;
                }
                break;
            case ActiveState::Data:
                if (!consume_data_state(&cursor)) {
                    return false;
                }
                break;
            }
        }
        return flush_character();
    }

private:
    bool append_character(char character) {
        if (character_buffer_.size() >= config_.maximum_token_bytes) {
            return fail_tokenizer(
                error_,
                "HTML tokenizer coalesced character token exceeds bounded byte limit");
        }
        character_buffer_.push_back(character);
        return true;
    }

    bool append_characters(std::string_view value) {
        if (character_buffer_.size() > config_.maximum_token_bytes ||
            value.size() > config_.maximum_token_bytes - character_buffer_.size()) {
            return fail_tokenizer(
                error_,
                "HTML tokenizer coalesced character token exceeds bounded byte limit");
        }
        character_buffer_.append(value.data(), value.size());
        return true;
    }

    bool account_token() {
        return increment_counter(
            &stats_->tokens_emitted,
            error_,
            "HTML tokenizer token");
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
                *error_ = "HTML tokenizer sink rejected character token";
            }
            return false;
        }
        if (!account_token() ||
            !increment_counter(
                &stats_->character_tokens_emitted,
                error_,
                "HTML tokenizer character-token")) {
            return false;
        }
        const auto emitted =
            static_cast<std::uint64_t>(character_buffer_.size());
        if (stats_->character_bytes_emitted >
            std::numeric_limits<std::uint64_t>::max() - emitted) {
            return fail_tokenizer(
                error_,
                "HTML tokenizer character-byte counter overflow");
        }
        stats_->character_bytes_emitted += emitted;
        character_buffer_.clear();
        return true;
    }

    bool emit_end_tag(std::string_view name) {
        if (!flush_character()) {
            return false;
        }
        if (name.size() > config_.maximum_token_bytes) {
            return fail_tokenizer(
                error_,
                "HTML tokenizer end-tag token exceeds bounded byte limit");
        }
        HtmlTokenizerV1Token token;
        token.kind = HtmlTokenizerV1TokenKind::EndTag;
        token.name.assign(name.data(), name.size());
        if (!sink_->on_token(token, error_)) {
            if (error_->empty()) {
                *error_ = "HTML tokenizer sink rejected end-tag token";
            }
            return false;
        }
        return account_token() &&
            increment_counter(
                &stats_->end_tags_emitted,
                error_,
                "HTML tokenizer end-tag");
    }

    bool emit_parse_error(std::size_t offset, std::string code) {
        HtmlTokenizerV1ParseError parse_error =
            position_error(input_, offset, std::move(code));
        if (!sink_->on_parse_error(parse_error, error_)) {
            if (error_->empty()) {
                *error_ = "HTML tokenizer sink rejected parse error";
            }
            return false;
        }
        return increment_counter(
            &stats_->parse_errors_emitted,
            error_,
            "HTML tokenizer parse-error");
    }

    bool consume_rcdata_reference(std::size_t* cursor) {
        if (cursor == nullptr || *cursor >= input_.size() || input_[*cursor] != '&') {
            return fail_tokenizer(
                error_,
                "HTML tokenizer RCDATA character-reference dispatch invariant failed");
        }
        const std::size_t reference_start = *cursor;
        HtmlTokenizerCharacterReferenceV1Stats reference_stats{};
        HtmlTokenizerCharacterReferenceV1Result reference_result{};
        const bool success = consume_html_character_reference_v1(
            input_,
            reference_start,
            HtmlTokenizerCharacterReferenceV1Context::Data,
            sink_,
            &reference_stats,
            &reference_result,
            error_);
        if (!add_counter(
                &stats_->parse_errors_emitted,
                reference_stats.parse_errors_emitted,
                error_,
                "HTML tokenizer RCDATA parse-error")) {
            return false;
        }
        if (!success) {
            return false;
        }
        if (reference_result.next_offset <= reference_start ||
            reference_result.next_offset > input_.size() ||
            reference_result.replacement_utf8.empty()) {
            return fail_tokenizer(
                error_,
                "HTML tokenizer RCDATA character-reference result invariant failed");
        }
        if (!append_characters(reference_result.replacement_utf8)) {
            return false;
        }
        *cursor = reference_result.next_offset;
        return true;
    }

    bool consume_text_state(std::size_t* cursor, bool rcdata) {
        const char character = input_[*cursor];
        if (rcdata && character == '&') {
            return consume_rcdata_reference(cursor);
        }
        if (character != '<') {
            if (!append_character(character)) {
                return false;
            }
            ++*cursor;
            return true;
        }
        if (*cursor + 1U >= input_.size() ||
            input_[*cursor + 1U] != '/') {
            if (!append_character('<')) {
                return false;
            }
            ++*cursor;
            return true;
        }

        // With no last-start-tag authority, no end-tag candidate can be
        // appropriate. Reconsume the slash/name bytes in the active text state
        // by publishing only the '<' now. This preserves literal </... input
        // without manufacturing an empty-name end tag.
        if (last_start_tag_.empty()) {
            if (!append_character('<')) {
                return false;
            }
            ++*cursor;
            return true;
        }

        const std::string prefix = "</" + last_start_tag_;
        if (prefix.size() > input_.size() - *cursor) {
            if (!append_characters(input_.substr(*cursor))) {
                return false;
            }
            *cursor = input_.size();
            return true;
        }
        if (!ascii_iequals_at(input_, *cursor, prefix)) {
            if (!append_character('<')) {
                return false;
            }
            ++*cursor;
            return true;
        }

        const std::size_t after_name = *cursor + prefix.size();
        if (after_name == input_.size()) {
            if (!append_characters(input_.substr(*cursor, prefix.size()))) {
                return false;
            }
            *cursor = after_name;
            return true;
        }

        const char trailing = input_[after_name];
        if (trailing == '>') {
            if (!emit_end_tag(last_start_tag_)) {
                return false;
            }
            state_ = ActiveState::Data;
            *cursor = after_name + 1U;
            return true;
        }
        if (ascii_space(trailing)) {
            std::size_t probe = after_name;
            while (probe < input_.size() && ascii_space(input_[probe])) {
                ++probe;
            }
            if (probe == input_.size()) {
                if (!flush_character() ||
                    !emit_parse_error(input_.size(), "eof-in-tag")) {
                    return false;
                }
                *cursor = input_.size();
                return true;
            }
            if (input_[probe] != '>') {
                return fail_tokenizer(
                    error_,
                    "HTML tokenizer attributes on text-state end tags are outside admitted v1 subset");
            }
            if (!emit_end_tag(last_start_tag_)) {
                return false;
            }
            state_ = ActiveState::Data;
            *cursor = probe + 1U;
            return true;
        }
        if (trailing == '/') {
            if (after_name + 1U == input_.size()) {
                if (!flush_character() ||
                    !emit_parse_error(input_.size(), "eof-in-tag")) {
                    return false;
                }
                *cursor = input_.size();
                return true;
            }
            return fail_tokenizer(
                error_,
                "HTML tokenizer self-closing text-state end-tag recovery is outside admitted v1 subset");
        }

        // The candidate name matched but was not followed by an appropriate
        // end-tag delimiter. Re-emit the candidate prefix as text and reconsume
        // the trailing byte in the active text state. This is required for
        // sequences such as </xmp</xmp>.
        if (!append_characters(input_.substr(*cursor, prefix.size()))) {
            return false;
        }
        *cursor = after_name;
        return true;
    }

    bool consume_data_state(std::size_t* cursor) {
        const char character = input_[*cursor];
        if (character == '&') {
            return fail_tokenizer(
                error_,
                "HTML tokenizer Data-state character references are outside admitted v1 subset");
        }
        if (character != '<') {
            if (!append_character(character)) {
                return false;
            }
            ++*cursor;
            return true;
        }
        if (*cursor + 2U >= input_.size() ||
            input_[*cursor + 1U] != '/') {
            return fail_tokenizer(
                error_,
                "HTML tokenizer Data-state start-tag/markup handling is outside admitted v1 subset");
        }

        std::size_t probe = *cursor + 2U;
        const std::size_t name_begin = probe;
        while (probe < input_.size() && tag_name_character(input_[probe])) {
            ++probe;
        }
        if (probe == name_begin) {
            return fail_tokenizer(
                error_,
                "HTML tokenizer Data-state end tag is missing a name");
        }
        const std::string name =
            ascii_lower_copy(input_.substr(name_begin, probe - name_begin));
        while (probe < input_.size() && ascii_space(input_[probe])) {
            ++probe;
        }
        if (probe == input_.size()) {
            if (!flush_character() ||
                !emit_parse_error(input_.size(), "eof-in-tag")) {
                return false;
            }
            *cursor = input_.size();
            return true;
        }
        if (input_[probe] != '>') {
            return fail_tokenizer(
                error_,
                "HTML tokenizer Data-state end-tag trailing syntax is outside admitted v1 subset");
        }
        if (!emit_end_tag(name)) {
            return false;
        }
        *cursor = probe + 1U;
        return true;
    }

    std::string_view input_;
    ActiveState state_{ActiveState::Plaintext};
    std::string last_start_tag_;
    HtmlTokenizerV1Config config_{};
    HtmlTokenizerV1Sink* sink_{nullptr};
    HtmlTokenizerV1Stats* stats_{nullptr};
    std::string* error_{nullptr};
    std::string character_buffer_;
};

} // namespace

bool tokenize_html_token_stream_v1(
    std::string_view input,
    HtmlTokenizerV1InitialState initial_state,
    std::string_view last_start_tag,
    HtmlTokenizerV1Config config,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerV1Stats* stats,
    std::string* error) {
    if (error == nullptr || sink == nullptr) {
        return false;
    }
    error->clear();

    HtmlTokenizerV1Stats local_stats{};
    local_stats.input_bytes = static_cast<std::uint64_t>(input.size());
    if (stats != nullptr) {
        *stats = local_stats;
    }
    if (!validate_config(config, error)) {
        return false;
    }
    if (input.size() > config.maximum_input_bytes) {
        return fail_tokenizer(
            error,
            "HTML tokenizer input exceeds bounded byte limit");
    }

    if (initial_state == HtmlTokenizerV1InitialState::ScriptData) {
        const bool success = run_script_data_canonical(
            input,
            last_start_tag,
            config,
            sink,
            &local_stats,
            error);
        if (stats != nullptr) {
            *stats = local_stats;
        }
        return success;
    }

    if (initial_state == HtmlTokenizerV1InitialState::Data) {
        const bool success = run_data_canonical(
            input,
            config,
            sink,
            &local_stats,
            error);
        if (stats != nullptr) {
            *stats = local_stats;
        }
        return success;
    }

    ActiveState active_state = ActiveState::Plaintext;
    switch (initial_state) {
    case HtmlTokenizerV1InitialState::Plaintext:
        active_state = ActiveState::Plaintext;
        break;
    case HtmlTokenizerV1InitialState::Rcdata:
        active_state = ActiveState::Rcdata;
        break;
    case HtmlTokenizerV1InitialState::Rawtext:
        active_state = ActiveState::Rawtext;
        break;
    case HtmlTokenizerV1InitialState::ScriptData:
        return fail_tokenizer(
            error,
            "HTML tokenizer Script-data dispatch invariant failed");
    case HtmlTokenizerV1InitialState::Data:
        return fail_tokenizer(
            error,
            "HTML tokenizer Data dispatch invariant failed");
    default:
        return fail_tokenizer(
            error,
            "HTML tokenizer initial state is invalid");
    }

    bool success = false;
    try {
        std::string normalized_last_start_tag;
        if (!validate_last_start_tag(
                last_start_tag,
                config,
                &normalized_last_start_tag,
                error)) {
            return false;
        }

        Tokenizer tokenizer(
            input,
            active_state,
            std::move(normalized_last_start_tag),
            config,
            sink,
            &local_stats,
            error);
        success = tokenizer.run();
    } catch (const std::bad_alloc&) {
        success = fail_tokenizer(
            error,
            "HTML tokenizer allocation failed within bounded v1 stream");
    }

    if (stats != nullptr) {
        *stats = local_stats;
    }
    return success;
}

} // namespace zevryon::massivedoc
