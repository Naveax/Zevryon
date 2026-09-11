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
constexpr std::string_view kReplacementCharacterUtf8 = "\xEF\xBF\xBD";

enum class ScriptState : std::uint8_t {
    Data,
    LessThanSign,
    EndTagOpen,
    EndTagName,
    EscapeStart,
    EscapeStartDash,
    Escaped,
    EscapedDash,
    EscapedDashDash,
    EscapedLessThanSign,
    EscapedEndTagOpen,
    EscapedEndTagName,
    DoubleEscapeStart,
    DoubleEscaped,
    DoubleEscapedDash,
    DoubleEscapedDashDash,
    DoubleEscapedLessThanSign,
    DoubleEscapeEnd,
};

bool fail_script(std::string* error, std::string message) {
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

bool increment_counter(
    std::uint64_t* value,
    std::string* error,
    std::string_view label) {
    if (*value == std::numeric_limits<std::uint64_t>::max()) {
        return fail_script(error, std::string(label) + " counter overflow");
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
        return fail_script(error, std::string(label) + " counter overflow");
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

bool validate_config(const HtmlTokenizerV1Config& config, std::string* error) {
    if (config.maximum_input_bytes == 0U ||
        config.maximum_input_bytes > kMaximumConfiguredInputBytes) {
        return fail_script(
            error,
            "HTML Script-data input bound is outside supported range");
    }
    if (config.maximum_token_bytes == 0U ||
        config.maximum_token_bytes > kMaximumConfiguredTokenBytes) {
        return fail_script(
            error,
            "HTML Script-data token bound is outside supported range");
    }
    return true;
}

bool normalize_last_start_tag(
    std::string_view value,
    const HtmlTokenizerV1Config& config,
    std::string* normalized,
    std::string* error) {
    if (value.size() > config.maximum_token_bytes) {
        return fail_script(
            error,
            "HTML Script-data last-start-tag exceeds bounded byte limit");
    }
    normalized->clear();
    normalized->reserve(value.size());
    for (char character : value) {
        if (!tag_name_character(character)) {
            return fail_script(
                error,
                "HTML Script-data last-start-tag contains unsupported byte");
        }
        normalized->push_back(ascii_lower(character));
    }
    return true;
}

class ScriptDataTokenizer final {
public:
    ScriptDataTokenizer(
        std::string_view input,
        std::string last_start_tag,
        HtmlTokenizerV1Config config,
        HtmlTokenizerV1Sink* sink,
        HtmlTokenizerScriptDataV1Stats* stats,
        HtmlTokenizerScriptDataV1Result* result,
        std::string* error)
        : input_(input),
          last_start_tag_(std::move(last_start_tag)),
          config_(config),
          sink_(sink),
          stats_(stats),
          result_(result),
          error_(error) {
        const std::size_t reserve =
            std::min<std::size_t>(config_.maximum_token_bytes, 4096U);
        character_buffer_.reserve(reserve);
        candidate_original_.reserve(std::min<std::size_t>(reserve, 256U));
        temporary_lower_.reserve(std::min<std::size_t>(reserve, 256U));
    }

    bool run() {
        std::size_t cursor = 0U;
        while (cursor < input_.size() && !done_) {
            if (!ascii_byte(input_[cursor])) {
                stats_->bytes_consumed = static_cast<std::uint64_t>(cursor);
                return fail_script(
                    error_,
                    "HTML Script-data non-ASCII preprocessing/location authority is not implemented");
            }
            if (!consume_state(&cursor)) {
                stats_->bytes_consumed = static_cast<std::uint64_t>(cursor);
                return false;
            }
            stats_->bytes_consumed = static_cast<std::uint64_t>(cursor);
        }

        if (!done_) {
            if (!finish_eof()) {
                stats_->bytes_consumed = static_cast<std::uint64_t>(input_.size());
                return false;
            }
            result_->next_offset = input_.size();
            result_->transitioned_to_data = false;
            stats_->bytes_consumed = static_cast<std::uint64_t>(input_.size());
        }
        return flush_character();
    }

private:
    bool consume_null(std::size_t* cursor, ScriptState next_state) {
        if (!emit_parse_error(*cursor, "unexpected-null-character") ||
            !append_characters(kReplacementCharacterUtf8)) {
            return false;
        }
        state_ = next_state;
        ++*cursor;
        return true;
    }

    bool append_character(char character) {
        if (character_buffer_.size() >= config_.maximum_token_bytes) {
            return fail_script(
                error_,
                "HTML Script-data coalesced character token exceeds bounded byte limit");
        }
        character_buffer_.push_back(character);
        return true;
    }

    bool append_characters(std::string_view value) {
        if (character_buffer_.size() > config_.maximum_token_bytes ||
            value.size() > config_.maximum_token_bytes - character_buffer_.size()) {
            return fail_script(
                error_,
                "HTML Script-data coalesced character token exceeds bounded byte limit");
        }
        character_buffer_.append(value.data(), value.size());
        return true;
    }

    bool append_candidate(char character) {
        if (candidate_original_.size() >= config_.maximum_token_bytes ||
            temporary_lower_.size() >= config_.maximum_token_bytes) {
            return fail_script(
                error_,
                "HTML Script-data end-tag candidate exceeds bounded byte limit");
        }
        candidate_original_.push_back(character);
        temporary_lower_.push_back(ascii_lower(character));
        return true;
    }

    bool append_temporary_lower(char character) {
        if (temporary_lower_.size() >= config_.maximum_token_bytes) {
            return fail_script(
                error_,
                "HTML Script-data temporary buffer exceeds bounded byte limit");
        }
        temporary_lower_.push_back(ascii_lower(character));
        return true;
    }

    void clear_candidate() {
        candidate_original_.clear();
        temporary_lower_.clear();
    }

    bool candidate_is_appropriate() const noexcept {
        return !last_start_tag_.empty() &&
            temporary_lower_ == last_start_tag_;
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
                *error_ = "HTML Script-data sink rejected character token";
            }
            return false;
        }
        if (!increment_counter(
                &stats_->tokens_emitted,
                error_,
                "HTML Script-data token") ||
            !increment_counter(
                &stats_->character_tokens_emitted,
                error_,
                "HTML Script-data character-token") ||
            !add_counter(
                &stats_->character_bytes_emitted,
                static_cast<std::uint64_t>(character_buffer_.size()),
                error_,
                "HTML Script-data character-byte")) {
            return false;
        }
        character_buffer_.clear();
        return true;
    }

    bool emit_end_tag() {
        if (!flush_character()) {
            return false;
        }
        HtmlTokenizerV1Token token;
        token.kind = HtmlTokenizerV1TokenKind::EndTag;
        token.name = last_start_tag_;
        if (!sink_->on_token(token, error_)) {
            if (error_->empty()) {
                *error_ = "HTML Script-data sink rejected end-tag token";
            }
            return false;
        }
        return increment_counter(
                   &stats_->tokens_emitted,
                   error_,
                   "HTML Script-data token") &&
            increment_counter(
                &stats_->end_tags_emitted,
                error_,
                "HTML Script-data end-tag");
    }

    bool emit_parse_error(std::size_t offset, std::string code) {
        HtmlTokenizerV1ParseError parse_error =
            position_error(input_, offset, std::move(code));
        if (!sink_->on_parse_error(parse_error, error_)) {
            if (error_->empty()) {
                *error_ = "HTML Script-data sink rejected parse error";
            }
            return false;
        }
        return increment_counter(
            &stats_->parse_errors_emitted,
            error_,
            "HTML Script-data parse-error");
    }

    bool fallback_end_tag_candidate(ScriptState fallback_state) {
        if (!append_characters("</") ||
            !append_characters(candidate_original_)) {
            return false;
        }
        clear_candidate();
        state_ = fallback_state;
        return true;
    }

    bool consume_end_tag_name(
        std::size_t* cursor,
        ScriptState fallback_state) {
        const char character = input_[*cursor];
        if (ascii_alpha(character)) {
            if (!append_candidate(character)) {
                return false;
            }
            ++*cursor;
            return true;
        }

        if (candidate_is_appropriate()) {
            if (character == '>') {
                if (!emit_end_tag()) {
                    return false;
                }
                ++*cursor;
                result_->next_offset = *cursor;
                result_->transitioned_to_data = true;
                done_ = true;
                return true;
            }
            if (ascii_space(character)) {
                std::size_t probe = *cursor;
                while (probe < input_.size() && ascii_space(input_[probe])) {
                    ++probe;
                }
                if (probe == input_.size()) {
                    if (!flush_character() ||
                        !emit_parse_error(input_.size(), "eof-in-tag")) {
                        return false;
                    }
                    *cursor = input_.size();
                    done_ = true;
                    result_->next_offset = input_.size();
                    result_->transitioned_to_data = false;
                    return true;
                }
                if (input_[probe] != '>') {
                    return fail_script(
                        error_,
                        "HTML Script-data attributes on appropriate end tags are outside admitted v1 subset");
                }
                if (!emit_end_tag()) {
                    return false;
                }
                *cursor = probe + 1U;
                result_->next_offset = *cursor;
                result_->transitioned_to_data = true;
                done_ = true;
                return true;
            }
            if (character == '/') {
                return fail_script(
                    error_,
                    "HTML Script-data self-closing appropriate end-tag recovery is outside admitted v1 subset");
            }
        }

        return fallback_end_tag_candidate(fallback_state);
    }

    bool consume_state(std::size_t* cursor) {
        const char character = input_[*cursor];
        switch (state_) {
        case ScriptState::Data:
            if (character == '\0') {
                return consume_null(cursor, ScriptState::Data);
            }
            if (character == '<') {
                state_ = ScriptState::LessThanSign;
                ++*cursor;
                return true;
            }
            if (!append_character(character)) {
                return false;
            }
            ++*cursor;
            return true;

        case ScriptState::LessThanSign:
            if (character == '/') {
                clear_candidate();
                state_ = ScriptState::EndTagOpen;
                ++*cursor;
                return true;
            }
            if (character == '!') {
                if (!append_characters("<!")) {
                    return false;
                }
                state_ = ScriptState::EscapeStart;
                ++*cursor;
                return true;
            }
            if (!append_character('<')) {
                return false;
            }
            state_ = ScriptState::Data;
            return true;

        case ScriptState::EndTagOpen:
            if (ascii_alpha(character)) {
                clear_candidate();
                state_ = ScriptState::EndTagName;
                return true;
            }
            if (!append_characters("</")) {
                return false;
            }
            state_ = ScriptState::Data;
            return true;

        case ScriptState::EndTagName:
            return consume_end_tag_name(cursor, ScriptState::Data);

        case ScriptState::EscapeStart:
            if (character == '-') {
                if (!append_character('-')) {
                    return false;
                }
                state_ = ScriptState::EscapeStartDash;
                ++*cursor;
                return true;
            }
            state_ = ScriptState::Data;
            return true;

        case ScriptState::EscapeStartDash:
            if (character == '-') {
                if (!append_character('-')) {
                    return false;
                }
                state_ = ScriptState::EscapedDashDash;
                ++*cursor;
                return true;
            }
            state_ = ScriptState::Data;
            return true;

        case ScriptState::Escaped:
            if (character == '\0') {
                return consume_null(cursor, ScriptState::Escaped);
            }
            if (character == '-') {
                if (!append_character('-')) {
                    return false;
                }
                state_ = ScriptState::EscapedDash;
                ++*cursor;
                return true;
            }
            if (character == '<') {
                state_ = ScriptState::EscapedLessThanSign;
                ++*cursor;
                return true;
            }
            if (!append_character(character)) {
                return false;
            }
            ++*cursor;
            return true;

        case ScriptState::EscapedDash:
            if (character == '\0') {
                return consume_null(cursor, ScriptState::Escaped);
            }
            if (character == '-') {
                if (!append_character('-')) {
                    return false;
                }
                state_ = ScriptState::EscapedDashDash;
                ++*cursor;
                return true;
            }
            if (character == '<') {
                state_ = ScriptState::EscapedLessThanSign;
                ++*cursor;
                return true;
            }
            if (!append_character(character)) {
                return false;
            }
            state_ = ScriptState::Escaped;
            ++*cursor;
            return true;

        case ScriptState::EscapedDashDash:
            if (character == '\0') {
                return consume_null(cursor, ScriptState::Escaped);
            }
            if (character == '-') {
                if (!append_character('-')) {
                    return false;
                }
                ++*cursor;
                return true;
            }
            if (character == '<') {
                state_ = ScriptState::EscapedLessThanSign;
                ++*cursor;
                return true;
            }
            if (character == '>') {
                if (!append_character('>')) {
                    return false;
                }
                state_ = ScriptState::Data;
                ++*cursor;
                return true;
            }
            if (!append_character(character)) {
                return false;
            }
            state_ = ScriptState::Escaped;
            ++*cursor;
            return true;

        case ScriptState::EscapedLessThanSign:
            if (character == '/') {
                clear_candidate();
                state_ = ScriptState::EscapedEndTagOpen;
                ++*cursor;
                return true;
            }
            if (ascii_alpha(character)) {
                temporary_lower_.clear();
                if (!append_character('<')) {
                    return false;
                }
                state_ = ScriptState::DoubleEscapeStart;
                return true;
            }
            if (!append_character('<')) {
                return false;
            }
            state_ = ScriptState::Escaped;
            return true;

        case ScriptState::EscapedEndTagOpen:
            if (ascii_alpha(character)) {
                clear_candidate();
                state_ = ScriptState::EscapedEndTagName;
                return true;
            }
            if (!append_characters("</")) {
                return false;
            }
            state_ = ScriptState::Escaped;
            return true;

        case ScriptState::EscapedEndTagName:
            return consume_end_tag_name(cursor, ScriptState::Escaped);

        case ScriptState::DoubleEscapeStart:
            if (ascii_space(character) || character == '/' || character == '>') {
                if (!append_character(character)) {
                    return false;
                }
                state_ = temporary_lower_ == "script"
                    ? ScriptState::DoubleEscaped
                    : ScriptState::Escaped;
                temporary_lower_.clear();
                ++*cursor;
                return true;
            }
            if (ascii_alpha(character)) {
                if (!append_temporary_lower(character) ||
                    !append_character(character)) {
                    return false;
                }
                ++*cursor;
                return true;
            }
            state_ = ScriptState::Escaped;
            return true;

        case ScriptState::DoubleEscaped:
            if (character == '\0') {
                return consume_null(cursor, ScriptState::DoubleEscaped);
            }
            if (character == '-') {
                if (!append_character('-')) {
                    return false;
                }
                state_ = ScriptState::DoubleEscapedDash;
                ++*cursor;
                return true;
            }
            if (character == '<') {
                if (!append_character('<')) {
                    return false;
                }
                state_ = ScriptState::DoubleEscapedLessThanSign;
                ++*cursor;
                return true;
            }
            if (!append_character(character)) {
                return false;
            }
            ++*cursor;
            return true;

        case ScriptState::DoubleEscapedDash:
            if (character == '\0') {
                return consume_null(cursor, ScriptState::DoubleEscaped);
            }
            if (character == '-') {
                if (!append_character('-')) {
                    return false;
                }
                state_ = ScriptState::DoubleEscapedDashDash;
                ++*cursor;
                return true;
            }
            if (character == '<') {
                if (!append_character('<')) {
                    return false;
                }
                state_ = ScriptState::DoubleEscapedLessThanSign;
                ++*cursor;
                return true;
            }
            if (!append_character(character)) {
                return false;
            }
            state_ = ScriptState::DoubleEscaped;
            ++*cursor;
            return true;

        case ScriptState::DoubleEscapedDashDash:
            if (character == '\0') {
                return consume_null(cursor, ScriptState::DoubleEscaped);
            }
            if (character == '-') {
                if (!append_character('-')) {
                    return false;
                }
                ++*cursor;
                return true;
            }
            if (character == '<') {
                if (!append_character('<')) {
                    return false;
                }
                state_ = ScriptState::DoubleEscapedLessThanSign;
                ++*cursor;
                return true;
            }
            if (character == '>') {
                if (!append_character('>')) {
                    return false;
                }
                state_ = ScriptState::Data;
                ++*cursor;
                return true;
            }
            if (!append_character(character)) {
                return false;
            }
            state_ = ScriptState::DoubleEscaped;
            ++*cursor;
            return true;

        case ScriptState::DoubleEscapedLessThanSign:
            if (character == '/') {
                if (!append_character('/')) {
                    return false;
                }
                temporary_lower_.clear();
                state_ = ScriptState::DoubleEscapeEnd;
                ++*cursor;
                return true;
            }
            state_ = ScriptState::DoubleEscaped;
            return true;

        case ScriptState::DoubleEscapeEnd:
            if (ascii_space(character) || character == '/' || character == '>') {
                if (!append_character(character)) {
                    return false;
                }
                state_ = temporary_lower_ == "script"
                    ? ScriptState::Escaped
                    : ScriptState::DoubleEscaped;
                temporary_lower_.clear();
                ++*cursor;
                return true;
            }
            if (ascii_alpha(character)) {
                if (!append_temporary_lower(character) ||
                    !append_character(character)) {
                    return false;
                }
                ++*cursor;
                return true;
            }
            state_ = ScriptState::DoubleEscaped;
            return true;
        }
        return fail_script(error_, "HTML Script-data internal state is invalid");
    }

    bool finish_eof() {
        bool eof_comment_like_error = false;
        switch (state_) {
        case ScriptState::Data:
        case ScriptState::EscapeStart:
        case ScriptState::EscapeStartDash:
            break;
        case ScriptState::LessThanSign:
            if (!append_character('<')) {
                return false;
            }
            break;
        case ScriptState::EndTagOpen:
            if (!append_characters("</")) {
                return false;
            }
            break;
        case ScriptState::EndTagName:
            if (!fallback_end_tag_candidate(ScriptState::Data)) {
                return false;
            }
            break;
        case ScriptState::Escaped:
        case ScriptState::EscapedDash:
        case ScriptState::EscapedDashDash:
        case ScriptState::DoubleEscapeStart:
        case ScriptState::DoubleEscaped:
        case ScriptState::DoubleEscapedDash:
        case ScriptState::DoubleEscapedDashDash:
        case ScriptState::DoubleEscapedLessThanSign:
        case ScriptState::DoubleEscapeEnd:
            eof_comment_like_error = true;
            break;
        case ScriptState::EscapedLessThanSign:
            if (!append_character('<')) {
                return false;
            }
            eof_comment_like_error = true;
            break;
        case ScriptState::EscapedEndTagOpen:
            if (!append_characters("</")) {
                return false;
            }
            eof_comment_like_error = true;
            break;
        case ScriptState::EscapedEndTagName:
            if (!fallback_end_tag_candidate(ScriptState::Escaped)) {
                return false;
            }
            eof_comment_like_error = true;
            break;
        }
        if (eof_comment_like_error &&
            !emit_parse_error(
                input_.size(),
                "eof-in-script-html-comment-like-text")) {
            return false;
        }
        return true;
    }

    std::string_view input_;
    std::string last_start_tag_;
    HtmlTokenizerV1Config config_{};
    HtmlTokenizerV1Sink* sink_{nullptr};
    HtmlTokenizerScriptDataV1Stats* stats_{nullptr};
    HtmlTokenizerScriptDataV1Result* result_{nullptr};
    std::string* error_{nullptr};
    ScriptState state_{ScriptState::Data};
    std::string character_buffer_;
    std::string candidate_original_;
    std::string temporary_lower_;
    bool done_{false};
};

} // namespace

bool consume_html_script_data_v1(
    std::string_view input,
    std::string_view last_start_tag,
    HtmlTokenizerV1Config config,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerScriptDataV1Stats* stats,
    HtmlTokenizerScriptDataV1Result* result,
    std::string* error) {
    if (sink == nullptr || result == nullptr || error == nullptr) {
        return false;
    }
    error->clear();

    HtmlTokenizerScriptDataV1Stats local_stats{};
    local_stats.input_bytes_available = static_cast<std::uint64_t>(input.size());
    *result = HtmlTokenizerScriptDataV1Result{};
    if (stats != nullptr) {
        *stats = local_stats;
    }
    if (!validate_config(config, error)) {
        return false;
    }
    if (input.size() > config.maximum_input_bytes) {
        return fail_script(
            error,
            "HTML Script-data input exceeds bounded byte limit");
    }

    bool success = false;
    try {
        std::string normalized_last_start_tag;
        if (!normalize_last_start_tag(
                last_start_tag,
                config,
                &normalized_last_start_tag,
                error)) {
            return false;
        }
        ScriptDataTokenizer tokenizer(
            input,
            std::move(normalized_last_start_tag),
            config,
            sink,
            &local_stats,
            result,
            error);
        success = tokenizer.run();
    } catch (const std::bad_alloc&) {
        success = fail_script(
            error,
            "HTML Script-data allocation failed within bounded v1 component");
    }

    if (stats != nullptr) {
        *stats = local_stats;
    }
    return success;
}

} // namespace zevryon::massivedoc
