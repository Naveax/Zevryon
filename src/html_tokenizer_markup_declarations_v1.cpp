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

bool ascii_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' ||
        value == '\r' || value == '\f';
}

bool ascii_control_parse_error(char value) noexcept {
    const auto byte = static_cast<unsigned char>(value);
    return (byte >= 0x01U && byte <= 0x08U) || byte == 0x0BU ||
        (byte >= 0x0EU && byte <= 0x1FU) || byte == 0x7FU;
}

char ascii_lower(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
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
            const unsigned char byte = static_cast<unsigned char>(input[index]);
            if ((byte & 0xC0U) == 0x80U) {
                continue;
            }
            result.column += (byte & 0xF8U) == 0xF0U ? 2U : 1U;
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
    enum class DoctypeState {
        AfterKeyword,
        BeforeName,
        Name,
        AfterName,
        AfterPublicKeyword,
        BeforePublicIdentifier,
        PublicIdentifierDoubleQuoted,
        PublicIdentifierSingleQuoted,
        AfterPublicIdentifier,
        BetweenPublicAndSystemIdentifiers,
        AfterSystemKeyword,
        BeforeSystemIdentifier,
        SystemIdentifierDoubleQuoted,
        SystemIdentifierSingleQuoted,
        AfterSystemIdentifier,
        Bogus,
    };

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

    bool emit_doctype(
        std::string name,
        std::string public_identifier,
        bool has_public_identifier,
        std::string system_identifier,
        bool has_system_identifier,
        bool force_quirks,
        bool has_doctype_name = true) {
        if (name.size() > config_.maximum_token_bytes ||
            public_identifier.size() > config_.maximum_token_bytes ||
            system_identifier.size() > config_.maximum_token_bytes ||
            name.size() + public_identifier.size() > config_.maximum_token_bytes ||
            name.size() + public_identifier.size() + system_identifier.size() >
                config_.maximum_token_bytes) {
            return fail_markup(
                error_,
                "HTML markup declaration DOCTYPE token exceeds bounded byte limit");
        }
        HtmlTokenizerV1Token token;
        token.kind = HtmlTokenizerV1TokenKind::Doctype;
        token.name = std::move(name);
        token.has_doctype_name = has_doctype_name;
        token.public_identifier = std::move(public_identifier);
        token.system_identifier = std::move(system_identifier);
        token.has_public_identifier = has_public_identifier;
        token.has_system_identifier = has_system_identifier;
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

    bool append_doctype_byte(
        std::string* destination,
        char value,
        std::string_view label) {
        if (destination->size() >= config_.maximum_token_bytes) {
            return fail_markup(
                error_,
                "HTML markup declaration DOCTYPE " + std::string(label) +
                    " exceeds bounded byte limit");
        }
        destination->push_back(value);
        return true;
    }

    bool fail_unsupported_doctype_ascii_boundary(
        bool original_missing_whitespace_before_name,
        DoctypeState state) {
        if (original_missing_whitespace_before_name || state == DoctypeState::AfterKeyword) {
            return fail_markup(
                error_,
                "HTML markup declaration malformed DOCTYPE-name transition is outside admitted v1 recovery");
        }
        if (state == DoctypeState::BeforeName) {
            return fail_markup(
                error_,
                "HTML markup declaration missing DOCTYPE name recovery is outside admitted v1 subset");
        }
        return fail_markup(
            error_,
            "HTML markup declaration PUBLIC/SYSTEM or malformed DOCTYPE recovery is outside admitted v1 subset");
    }

    bool observe_doctype_character(
        std::size_t cursor,
        bool original_missing_whitespace_before_name,
        DoctypeState state) {
        const char value = input_[cursor];
        if (value == '\0') {
            return fail_markup(
                error_,
                "HTML markup declaration input preprocessing/NUL replacement is not implemented");
        }
        if (!ascii_byte(value)) {
            if (state == DoctypeState::AfterKeyword ||
                state == DoctypeState::BeforeName ||
                state == DoctypeState::Name ||
                state == DoctypeState::PublicIdentifierDoubleQuoted ||
                state == DoctypeState::PublicIdentifierSingleQuoted ||
                state == DoctypeState::SystemIdentifierDoubleQuoted ||
                state == DoctypeState::SystemIdentifierSingleQuoted) {
                return true;
            }
            return fail_unsupported_doctype_ascii_boundary(
                original_missing_whitespace_before_name,
                state);
        }
        if (ascii_control_parse_error(value) &&
            !emit_parse_error(cursor, "control-character-in-input-stream")) {
            return false;
        }
        return true;
    }

    bool finish_doctype(
        std::size_t next_offset,
        std::string name,
        std::string public_identifier,
        bool has_public_identifier,
        std::string system_identifier,
        bool has_system_identifier,
        bool force_quirks,
        bool has_doctype_name = true) {
        if (!emit_doctype(
                std::move(name),
                std::move(public_identifier),
                has_public_identifier,
                std::move(system_identifier),
                has_system_identifier,
                force_quirks,
                has_doctype_name)) {
            return false;
        }
        *next_offset_ = next_offset;
        return true;
    }

    bool consume_doctype() {
        std::size_t cursor = offset_ + 9U;
        DoctypeState state = DoctypeState::AfterKeyword;
        bool skip_observe_once = false;
        bool original_missing_whitespace_before_name = false;
        bool force_quirks = false;
        bool has_public_identifier = false;
        bool has_system_identifier = false;
        std::string name;
        std::string public_identifier;
        std::string system_identifier;

        for (;;) {
            if (cursor >= input_.size()) {
                if (state == DoctypeState::AfterKeyword ||
                    state == DoctypeState::BeforeName) {
                    if (!emit_parse_error(input_.size(), "eof-in-doctype")) {
                        return false;
                    }
                    force_quirks = true;
                    return finish_doctype(
                        input_.size(),
                        std::move(name),
                        std::move(public_identifier),
                        has_public_identifier,
                        std::move(system_identifier),
                        has_system_identifier,
                        force_quirks,
                        false);
                }
                if (state != DoctypeState::Bogus) {
                    if (!emit_parse_error(input_.size(), "eof-in-doctype")) {
                        return false;
                    }
                    force_quirks = true;
                }
                return finish_doctype(
                    input_.size(),
                    std::move(name),
                    std::move(public_identifier),
                    has_public_identifier,
                    std::move(system_identifier),
                    has_system_identifier,
                    force_quirks);
            }

            if (!skip_observe_once) {
                if (!observe_doctype_character(
                        cursor,
                        original_missing_whitespace_before_name,
                        state)) {
                    return false;
                }
            } else {
                skip_observe_once = false;
            }

            const char value = input_[cursor];
            switch (state) {
            case DoctypeState::AfterKeyword:
                if (ascii_space(value)) {
                    state = DoctypeState::BeforeName;
                    ++cursor;
                    break;
                }
                if (value == '>') {
                    if (!emit_parse_error(cursor, "missing-doctype-name")) {
                        return false;
                    }
                    force_quirks = true;
                    return finish_doctype(
                        cursor + 1U,
                        std::move(name),
                        std::move(public_identifier),
                        has_public_identifier,
                        std::move(system_identifier),
                        has_system_identifier,
                        force_quirks,
                        false);
                }
                if (!emit_parse_error(cursor, "missing-whitespace-before-doctype-name")) {
                    return false;
                }
                original_missing_whitespace_before_name = true;
                state = DoctypeState::BeforeName;
                skip_observe_once = true;
                break;

            case DoctypeState::BeforeName:
                if (ascii_space(value)) {
                    ++cursor;
                    break;
                }
                if (value == '>') {
                    if (!emit_parse_error(cursor, "missing-doctype-name")) {
                        return false;
                    }
                    force_quirks = true;
                    return finish_doctype(
                        cursor + 1U,
                        std::move(name),
                        std::move(public_identifier),
                        has_public_identifier,
                        std::move(system_identifier),
                        has_system_identifier,
                        force_quirks,
                        false);
                }
                if (!append_doctype_byte(&name, ascii_lower(value), "name")) {
                    return false;
                }
                state = DoctypeState::Name;
                ++cursor;
                break;

            case DoctypeState::Name:
                if (ascii_space(value)) {
                    state = DoctypeState::AfterName;
                    ++cursor;
                    break;
                }
                if (value == '>') {
                    return finish_doctype(
                        cursor + 1U,
                        std::move(name),
                        std::move(public_identifier),
                        has_public_identifier,
                        std::move(system_identifier),
                        has_system_identifier,
                        force_quirks);
                }
                if (!append_doctype_byte(&name, ascii_lower(value), "name")) {
                    return false;
                }
                ++cursor;
                break;

            case DoctypeState::AfterName:
                if (ascii_space(value)) {
                    ++cursor;
                    break;
                }
                if (value == '>') {
                    return finish_doctype(
                        cursor + 1U,
                        std::move(name),
                        std::move(public_identifier),
                        has_public_identifier,
                        std::move(system_identifier),
                        has_system_identifier,
                        force_quirks);
                }
                if (ascii_iequals_at(input_, cursor, "PUBLIC")) {
                    cursor += 6U;
                    state = DoctypeState::AfterPublicKeyword;
                    break;
                }
                if (ascii_iequals_at(input_, cursor, "SYSTEM")) {
                    cursor += 6U;
                    state = DoctypeState::AfterSystemKeyword;
                    break;
                }
                if (!emit_parse_error(cursor, "invalid-character-sequence-after-doctype-name")) {
                    return false;
                }
                force_quirks = true;
                state = DoctypeState::Bogus;
                ++cursor;
                break;

            case DoctypeState::AfterPublicKeyword:
                if (ascii_space(value)) {
                    state = DoctypeState::BeforePublicIdentifier;
                    ++cursor;
                    break;
                }
                if (value == '"' || value == '\'') {
                    if (!emit_parse_error(
                            cursor,
                            "missing-whitespace-after-doctype-public-keyword")) {
                        return false;
                    }
                    has_public_identifier = true;
                    state = value == '"'
                        ? DoctypeState::PublicIdentifierDoubleQuoted
                        : DoctypeState::PublicIdentifierSingleQuoted;
                    ++cursor;
                    break;
                }
                if (value == '>') {
                    if (!emit_parse_error(cursor, "missing-doctype-public-identifier")) {
                        return false;
                    }
                    force_quirks = true;
                    return finish_doctype(
                        cursor + 1U,
                        std::move(name),
                        std::move(public_identifier),
                        has_public_identifier,
                        std::move(system_identifier),
                        has_system_identifier,
                        force_quirks);
                }
                if (!emit_parse_error(cursor, "missing-quote-before-doctype-public-identifier")) {
                    return false;
                }
                force_quirks = true;
                state = DoctypeState::Bogus;
                ++cursor;
                break;

            case DoctypeState::BeforePublicIdentifier:
                if (ascii_space(value)) {
                    ++cursor;
                    break;
                }
                if (value == '"' || value == '\'') {
                    has_public_identifier = true;
                    state = value == '"'
                        ? DoctypeState::PublicIdentifierDoubleQuoted
                        : DoctypeState::PublicIdentifierSingleQuoted;
                    ++cursor;
                    break;
                }
                if (value == '>') {
                    if (!emit_parse_error(cursor, "missing-doctype-public-identifier")) {
                        return false;
                    }
                    force_quirks = true;
                    return finish_doctype(
                        cursor + 1U,
                        std::move(name),
                        std::move(public_identifier),
                        has_public_identifier,
                        std::move(system_identifier),
                        has_system_identifier,
                        force_quirks);
                }
                if (!emit_parse_error(cursor, "missing-quote-before-doctype-public-identifier")) {
                    return false;
                }
                force_quirks = true;
                state = DoctypeState::Bogus;
                ++cursor;
                break;

            case DoctypeState::PublicIdentifierDoubleQuoted:
            case DoctypeState::PublicIdentifierSingleQuoted: {
                const char quote = state == DoctypeState::PublicIdentifierDoubleQuoted ? '"' : '\'';
                if (value == quote) {
                    state = DoctypeState::AfterPublicIdentifier;
                    ++cursor;
                    break;
                }
                if (value == '>') {
                    if (!emit_parse_error(cursor, "abrupt-doctype-public-identifier")) {
                        return false;
                    }
                    force_quirks = true;
                    return finish_doctype(
                        cursor + 1U,
                        std::move(name),
                        std::move(public_identifier),
                        has_public_identifier,
                        std::move(system_identifier),
                        has_system_identifier,
                        force_quirks);
                }
                if (!append_doctype_byte(&public_identifier, value, "public identifier")) {
                    return false;
                }
                ++cursor;
                break;
            }

            case DoctypeState::AfterPublicIdentifier:
                if (ascii_space(value)) {
                    state = DoctypeState::BetweenPublicAndSystemIdentifiers;
                    ++cursor;
                    break;
                }
                if (value == '>') {
                    return finish_doctype(
                        cursor + 1U,
                        std::move(name),
                        std::move(public_identifier),
                        has_public_identifier,
                        std::move(system_identifier),
                        has_system_identifier,
                        force_quirks);
                }
                if (value == '"' || value == '\'') {
                    if (!emit_parse_error(
                            cursor,
                            "missing-whitespace-between-doctype-public-and-system-identifiers")) {
                        return false;
                    }
                    has_system_identifier = true;
                    state = value == '"'
                        ? DoctypeState::SystemIdentifierDoubleQuoted
                        : DoctypeState::SystemIdentifierSingleQuoted;
                    ++cursor;
                    break;
                }
                if (!emit_parse_error(cursor, "missing-quote-before-doctype-system-identifier")) {
                    return false;
                }
                force_quirks = true;
                state = DoctypeState::Bogus;
                ++cursor;
                break;

            case DoctypeState::BetweenPublicAndSystemIdentifiers:
                if (ascii_space(value)) {
                    ++cursor;
                    break;
                }
                if (value == '>') {
                    return finish_doctype(
                        cursor + 1U,
                        std::move(name),
                        std::move(public_identifier),
                        has_public_identifier,
                        std::move(system_identifier),
                        has_system_identifier,
                        force_quirks);
                }
                if (value == '"' || value == '\'') {
                    has_system_identifier = true;
                    state = value == '"'
                        ? DoctypeState::SystemIdentifierDoubleQuoted
                        : DoctypeState::SystemIdentifierSingleQuoted;
                    ++cursor;
                    break;
                }
                if (!emit_parse_error(cursor, "missing-quote-before-doctype-system-identifier")) {
                    return false;
                }
                force_quirks = true;
                state = DoctypeState::Bogus;
                ++cursor;
                break;

            case DoctypeState::AfterSystemKeyword:
                if (ascii_space(value)) {
                    state = DoctypeState::BeforeSystemIdentifier;
                    ++cursor;
                    break;
                }
                if (value == '"' || value == '\'') {
                    if (!emit_parse_error(
                            cursor,
                            "missing-whitespace-after-doctype-system-keyword")) {
                        return false;
                    }
                    has_system_identifier = true;
                    state = value == '"'
                        ? DoctypeState::SystemIdentifierDoubleQuoted
                        : DoctypeState::SystemIdentifierSingleQuoted;
                    ++cursor;
                    break;
                }
                if (value == '>') {
                    if (!emit_parse_error(cursor, "missing-doctype-system-identifier")) {
                        return false;
                    }
                    force_quirks = true;
                    return finish_doctype(
                        cursor + 1U,
                        std::move(name),
                        std::move(public_identifier),
                        has_public_identifier,
                        std::move(system_identifier),
                        has_system_identifier,
                        force_quirks);
                }
                if (!emit_parse_error(cursor, "missing-quote-before-doctype-system-identifier")) {
                    return false;
                }
                force_quirks = true;
                state = DoctypeState::Bogus;
                ++cursor;
                break;

            case DoctypeState::BeforeSystemIdentifier:
                if (ascii_space(value)) {
                    ++cursor;
                    break;
                }
                if (value == '"' || value == '\'') {
                    has_system_identifier = true;
                    state = value == '"'
                        ? DoctypeState::SystemIdentifierDoubleQuoted
                        : DoctypeState::SystemIdentifierSingleQuoted;
                    ++cursor;
                    break;
                }
                if (value == '>') {
                    if (!emit_parse_error(cursor, "missing-doctype-system-identifier")) {
                        return false;
                    }
                    force_quirks = true;
                    return finish_doctype(
                        cursor + 1U,
                        std::move(name),
                        std::move(public_identifier),
                        has_public_identifier,
                        std::move(system_identifier),
                        has_system_identifier,
                        force_quirks);
                }
                if (!emit_parse_error(cursor, "missing-quote-before-doctype-system-identifier")) {
                    return false;
                }
                force_quirks = true;
                state = DoctypeState::Bogus;
                ++cursor;
                break;

            case DoctypeState::SystemIdentifierDoubleQuoted:
            case DoctypeState::SystemIdentifierSingleQuoted: {
                const char quote = state == DoctypeState::SystemIdentifierDoubleQuoted ? '"' : '\'';
                if (value == quote) {
                    state = DoctypeState::AfterSystemIdentifier;
                    ++cursor;
                    break;
                }
                if (value == '>') {
                    if (!emit_parse_error(cursor, "abrupt-doctype-system-identifier")) {
                        return false;
                    }
                    force_quirks = true;
                    return finish_doctype(
                        cursor + 1U,
                        std::move(name),
                        std::move(public_identifier),
                        has_public_identifier,
                        std::move(system_identifier),
                        has_system_identifier,
                        force_quirks);
                }
                if (!append_doctype_byte(&system_identifier, value, "system identifier")) {
                    return false;
                }
                ++cursor;
                break;
            }

            case DoctypeState::AfterSystemIdentifier:
                if (ascii_space(value)) {
                    ++cursor;
                    break;
                }
                if (value == '>') {
                    return finish_doctype(
                        cursor + 1U,
                        std::move(name),
                        std::move(public_identifier),
                        has_public_identifier,
                        std::move(system_identifier),
                        has_system_identifier,
                        force_quirks);
                }
                if (!emit_parse_error(cursor, "unexpected-character-after-doctype-system-identifier")) {
                    return false;
                }
                state = DoctypeState::Bogus;
                ++cursor;
                break;

            case DoctypeState::Bogus:
                if (value == '>') {
                    return finish_doctype(
                        cursor + 1U,
                        std::move(name),
                        std::move(public_identifier),
                        has_public_identifier,
                        std::move(system_identifier),
                        has_system_identifier,
                        force_quirks);
                }
                ++cursor;
                break;
            }
        }
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
