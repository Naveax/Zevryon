#include "html_tokenizer_markup_declarations_v1.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace zevryon::massivedoc {
namespace {

constexpr std::size_t kMaximumConfiguredTokenBytes = 1024U * 1024U;

bool fail_markup(std::string* error, std::string message) {
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

bool doctype_name_character(char value) noexcept {
    return ascii_alpha(value) || ascii_digit(value) || value == '-' ||
        value == '_' || value == ':';
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
        return fail_markup(error, std::string(label) + " counter overflow");
    }
    ++*value;
    return true;
}

bool validate_ascii_span(
    std::string_view value,
    std::string* error,
    std::string_view label) {
    for (char character : value) {
        if (character == '\0') {
            return fail_markup(
                error,
                "HTML markup declaration input preprocessing/NUL replacement is not implemented");
        }
        if (!ascii_byte(character)) {
            return fail_markup(
                error,
                std::string("HTML markup declaration non-ASCII ") +
                    std::string(label) + " authority is not implemented");
        }
    }
    return true;
}

class MarkupDeclarationTokenizer final {
public:
    MarkupDeclarationTokenizer(
        std::string_view input,
        std::size_t offset,
        HtmlTokenizerMarkupDeclarationsV1Config config,
        HtmlTokenizerV1Sink* sink,
        HtmlTokenizerMarkupDeclarationsV1Stats* stats,
        std::size_t* next_offset,
        std::string* error)
        : input_(input),
          offset_(offset),
          config_(config),
          sink_(sink),
          stats_(stats),
          next_offset_(next_offset),
          error_(error) {}

    bool run() {
        if (offset_ > input_.size() || input_.size() - offset_ < 2U ||
            input_[offset_] != '<' || input_[offset_ + 1U] != '!') {
            return fail_markup(
                error_,
                "HTML markup declaration consumer requires a <! declaration open");
        }
        if (input_.substr(offset_, 4U) == "<!--") {
            return consume_comment();
        }
        if (ascii_iequals_at(input_, offset_, "<!DOCTYPE")) {
            return consume_doctype();
        }
        return consume_bogus_comment();
    }

private:
    bool emit_parse_error(std::size_t offset, std::string code) {
        HtmlTokenizerV1ParseError parse_error =
            position_error(input_, offset, std::move(code));
        if (!sink_->on_parse_error(parse_error, error_)) {
            if (error_->empty()) {
                *error_ = "HTML markup declaration sink rejected parse error";
            }
            return false;
        }
        return increment_counter(
            &stats_->parse_errors_emitted,
            error_,
            "HTML markup declaration parse-error");
    }

    bool emit_comment(std::string_view data) {
        if (data.size() > config_.maximum_token_bytes) {
            return fail_markup(
                error_,
                "HTML markup declaration comment token exceeds bounded byte limit");
        }
        if (!validate_ascii_span(data, error_, "comment")) {
            return false;
        }
        HtmlTokenizerV1Token token;
        token.kind = HtmlTokenizerV1TokenKind::Comment;
        token.data.assign(data.data(), data.size());
        if (!sink_->on_token(token, error_)) {
            if (error_->empty()) {
                *error_ = "HTML markup declaration sink rejected comment token";
            }
            return false;
        }
        return increment_counter(
                   &stats_->tokens_emitted,
                   error_,
                   "HTML markup declaration token") &&
            increment_counter(
                &stats_->comment_tokens_emitted,
                error_,
                "HTML markup declaration comment-token");
    }

    bool emit_doctype(std::string name, bool force_quirks) {
        if (name.size() > config_.maximum_token_bytes) {
            return fail_markup(
                error_,
                "HTML markup declaration DOCTYPE token exceeds bounded byte limit");
        }
        HtmlTokenizerV1Token token;
        token.kind = HtmlTokenizerV1TokenKind::Doctype;
        token.name = std::move(name);
        token.force_quirks = force_quirks;
        if (!sink_->on_token(token, error_)) {
            if (error_->empty()) {
                *error_ = "HTML markup declaration sink rejected DOCTYPE token";
            }
            return false;
        }
        return increment_counter(
                   &stats_->tokens_emitted,
                   error_,
                   "HTML markup declaration token") &&
            increment_counter(
                &stats_->doctype_tokens_emitted,
                error_,
                "HTML markup declaration DOCTYPE-token");
    }

    bool consume_doctype() {
        std::size_t cursor = offset_ + 9U;
        if (cursor >= input_.size() || !ascii_space(input_[cursor])) {
            return fail_markup(
                error_,
                "HTML markup declaration malformed DOCTYPE-name transition is outside admitted v1 recovery");
        }
        while (cursor < input_.size() && ascii_space(input_[cursor])) {
            ++cursor;
        }
        if (cursor >= input_.size()) {
            return fail_markup(
                error_,
                "HTML markup declaration missing DOCTYPE name recovery is outside admitted v1 subset");
        }

        std::string name;
        while (cursor < input_.size() && doctype_name_character(input_[cursor])) {
            if (name.size() >= config_.maximum_token_bytes) {
                return fail_markup(
                    error_,
                    "HTML markup declaration DOCTYPE name exceeds bounded byte limit");
            }
            name.push_back(ascii_lower(input_[cursor]));
            ++cursor;
        }
        if (name.empty()) {
            return fail_markup(
                error_,
                "HTML markup declaration missing DOCTYPE name recovery is outside admitted v1 subset");
        }

        if (cursor == input_.size()) {
            if (!emit_parse_error(input_.size(), "eof-in-doctype") ||
                !emit_doctype(std::move(name), true)) {
                return false;
            }
            *next_offset_ = input_.size();
            return true;
        }

        while (cursor < input_.size() && ascii_space(input_[cursor])) {
            ++cursor;
        }
        if (cursor == input_.size()) {
            if (!emit_parse_error(input_.size(), "eof-in-doctype") ||
                !emit_doctype(std::move(name), true)) {
                return false;
            }
            *next_offset_ = input_.size();
            return true;
        }
        if (input_[cursor] != '>') {
            return fail_markup(
                error_,
                "HTML markup declaration PUBLIC/SYSTEM or malformed DOCTYPE recovery is outside admitted v1 subset");
        }
        if (!emit_doctype(std::move(name), false)) {
            return false;
        }
        *next_offset_ = cursor + 1U;
        return true;
    }

    bool consume_bogus_comment() {
        const std::size_t data_begin = offset_ + 2U;
        if (!emit_parse_error(data_begin, "incorrectly-opened-comment")) {
            return false;
        }
        std::size_t cursor = data_begin;
        while (cursor < input_.size() && input_[cursor] != '>') {
            if (input_[cursor] == '\0') {
                return fail_markup(
                    error_,
                    "HTML markup declaration input preprocessing/NUL replacement is not implemented");
            }
            if (!ascii_byte(input_[cursor])) {
                return fail_markup(
                    error_,
                    "HTML markup declaration non-ASCII bogus-comment authority is not implemented");
            }
            ++cursor;
        }
        const std::string_view data = input_.substr(data_begin, cursor - data_begin);
        if (!emit_comment(data)) {
            return false;
        }
        *next_offset_ = cursor < input_.size() ? cursor + 1U : cursor;
        return true;
    }

    bool consume_comment() {
        const std::size_t data_begin = offset_ + 4U;
        if (data_begin >= input_.size()) {
            if (!emit_parse_error(input_.size(), "eof-in-comment") ||
                !emit_comment({})) {
                return false;
            }
            *next_offset_ = input_.size();
            return true;
        }

        if (input_[data_begin] == '>') {
            if (!emit_parse_error(
                    data_begin,
                    "abrupt-closing-of-empty-comment") ||
                !emit_comment({})) {
                return false;
            }
            *next_offset_ = data_begin + 1U;
            return true;
        }
        if (input_[data_begin] == '-' &&
            data_begin + 1U < input_.size() &&
            input_[data_begin + 1U] == '>') {
            if (!emit_parse_error(
                    data_begin + 1U,
                    "abrupt-closing-of-empty-comment") ||
                !emit_comment({})) {
                return false;
            }
            *next_offset_ = data_begin + 2U;
            return true;
        }

        const std::size_t close = input_.find("-->", data_begin);
        if (close != std::string_view::npos) {
            const std::size_t nested = input_.find("<!--", data_begin);
            if (nested != std::string_view::npos && nested + 4U <= close) {
                if (!emit_parse_error(nested + 4U, "nested-comment")) {
                    return false;
                }
            }
            const std::string_view data = input_.substr(data_begin, close - data_begin);
            if (!emit_comment(data)) {
                return false;
            }
            *next_offset_ = close + 3U;
            return true;
        }

        std::size_t data_end = input_.size();
        if (data_end >= data_begin + 2U &&
            input_[data_end - 2U] == '-' && input_[data_end - 1U] == '-') {
            data_end -= 2U;
        }
        const std::string_view data = input_.substr(data_begin, data_end - data_begin);
        if (!emit_parse_error(input_.size(), "eof-in-comment") ||
            !emit_comment(data)) {
            return false;
        }
        *next_offset_ = input_.size();
        return true;
    }

    std::string_view input_;
    std::size_t offset_{0U};
    HtmlTokenizerMarkupDeclarationsV1Config config_{};
    HtmlTokenizerV1Sink* sink_{nullptr};
    HtmlTokenizerMarkupDeclarationsV1Stats* stats_{nullptr};
    std::size_t* next_offset_{nullptr};
    std::string* error_{nullptr};
};

} // namespace

bool consume_html_markup_declaration_v1(
    std::string_view input,
    std::size_t markup_open_offset,
    HtmlTokenizerMarkupDeclarationsV1Config config,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerMarkupDeclarationsV1Stats* stats,
    std::size_t* next_offset,
    std::string* error) {
    if (sink == nullptr || next_offset == nullptr || error == nullptr) {
        return false;
    }
    error->clear();
    *next_offset = markup_open_offset;

    HtmlTokenizerMarkupDeclarationsV1Stats local_stats{};
    if (stats != nullptr) {
        *stats = local_stats;
    }
    if (config.maximum_token_bytes == 0U ||
        config.maximum_token_bytes > kMaximumConfiguredTokenBytes) {
        return fail_markup(
            error,
            "HTML markup declaration token bound is outside supported range");
    }

    bool success = false;
    try {
        MarkupDeclarationTokenizer tokenizer(
            input,
            markup_open_offset,
            config,
            sink,
            &local_stats,
            next_offset,
            error);
        success = tokenizer.run();
    } catch (const std::bad_alloc&) {
        success = fail_markup(
            error,
            "HTML markup declaration allocation failed within bounded v1 slice");
    }

    if (stats != nullptr) {
        *stats = local_stats;
    }
    return success;
}

} // namespace zevryon::massivedoc
