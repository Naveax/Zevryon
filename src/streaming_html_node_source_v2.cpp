#include "streaming_html_node_source_v2.hpp"

#include "ledger_memory_resource.hpp"
#include "logical_node_source.hpp"
#include "logical_node_source_v2.hpp"
#include "massivedoc_store.hpp"
#include "resource_ledger.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zevryon::massivedoc {
namespace {

constexpr std::size_t kMaximumConfiguredTokenBytes = 16U * 1024U * 1024U;
constexpr std::uint32_t kMaximumConfiguredAttributes = 65'536U;
constexpr std::uint32_t kMaximumConfiguredDepth = 65'536U;
constexpr std::size_t kMaximumConfiguredWorkingSetBytes = 512U * 1024U * 1024U;

using PmrString = std::pmr::string;

template <typename String>
std::string_view view(const String& value) noexcept {
    return std::string_view(value.data(), value.size());
}

std::string owned(std::string_view value) {
    return std::string(value.data(), value.size());
}

template <typename String>
std::string owned(const String& value) {
    return owned(view(value));
}

bool fail_html_v2(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool ascii_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' ||
        value == '\r' || value == '\f';
}

bool ascii_alpha(char value) noexcept {
    return (value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z');
}

bool ascii_digit(char value) noexcept {
    return value >= '0' && value <= '9';
}

char ascii_lower(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

bool ascii_iequals(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

bool tag_name_character(char value) noexcept {
    return ascii_alpha(value) || ascii_digit(value) || value == '-';
}

bool attribute_name_character(char value) noexcept {
    return tag_name_character(value) || value == '_' || value == '.';
}

bool void_element(std::string_view tag) noexcept {
    constexpr std::string_view values[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr"};
    for (const std::string_view value : values) {
        if (tag == value) {
            return true;
        }
    }
    return false;
}

bool supported_raw_text_element(std::string_view tag) noexcept {
    constexpr std::string_view values[] = {
        "style", "xmp", "iframe", "noembed", "noframes"};
    for (const std::string_view value : values) {
        if (tag == value) {
            return true;
        }
    }
    return false;
}

bool supported_rcdata_element(std::string_view tag) noexcept {
    return tag == "title" || tag == "textarea";
}

bool unsupported_special_text_element(std::string_view tag) noexcept {
    constexpr std::string_view values[] = {
        "script", "plaintext", "noscript"};
    for (const std::string_view value : values) {
        if (tag == value) {
            return true;
        }
    }
    return false;
}

bool unsupported_foreign_root(std::string_view tag) noexcept {
    return tag == "svg" || tag == "math";
}

void ascii_lower_assign(std::string_view input, PmrString* output) {
    output->clear();
    output->reserve(input.size());
    for (const char character : input) {
        output->push_back(ascii_lower(character));
    }
}

bool append_utf8(std::uint32_t codepoint, PmrString* output) {
    if (output == nullptr || codepoint == 0U || codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
        return false;
    }
    if (codepoint <= 0x7fU) {
        output->push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        output->push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        output->push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        output->push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        output->push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output->push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        output->push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
        output->push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
        output->push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output->push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
    return true;
}

bool decode_character_references(
    std::string_view input,
    PmrString* output,
    std::string* error) {
    output->clear();
    output->reserve(input.size());
    for (std::size_t index = 0U; index < input.size();) {
        if (input[index] != '&') {
            output->push_back(input[index++]);
            continue;
        }
        const std::size_t semicolon = input.find(';', index + 1U);
        if (semicolon == std::string_view::npos || semicolon - index > 32U) {
            return fail_html_v2(
                error,
                "unterminated or oversized HTML character reference");
        }
        const std::string_view entity =
            input.substr(index + 1U, semicolon - index - 1U);
        if (entity == "amp") {
            output->push_back('&');
        } else if (entity == "lt") {
            output->push_back('<');
        } else if (entity == "gt") {
            output->push_back('>');
        } else if (entity == "quot") {
            output->push_back('"');
        } else if (entity == "apos") {
            output->push_back('\'');
        } else if (!entity.empty() && entity.front() == '#') {
            std::size_t cursor = 1U;
            unsigned base = 10U;
            if (cursor < entity.size() &&
                (entity[cursor] == 'x' || entity[cursor] == 'X')) {
                base = 16U;
                ++cursor;
            }
            if (cursor == entity.size()) {
                return fail_html_v2(error, "empty numeric HTML character reference");
            }
            std::uint32_t codepoint = 0U;
            for (; cursor < entity.size(); ++cursor) {
                const char character = entity[cursor];
                unsigned digit = 0U;
                if (character >= '0' && character <= '9') {
                    digit = static_cast<unsigned>(character - '0');
                } else if (base == 16U && character >= 'a' && character <= 'f') {
                    digit = static_cast<unsigned>(character - 'a' + 10);
                } else if (base == 16U && character >= 'A' && character <= 'F') {
                    digit = static_cast<unsigned>(character - 'A' + 10);
                } else {
                    return fail_html_v2(error, "invalid numeric HTML character reference");
                }
                if (digit >= base || codepoint > (0x10ffffU - digit) / base) {
                    return fail_html_v2(
                        error,
                        "HTML character reference overflows Unicode range");
                }
                codepoint = codepoint * base + digit;
            }
            if (!append_utf8(codepoint, output)) {
                return fail_html_v2(
                    error,
                    "invalid Unicode scalar in HTML character reference");
            }
        } else {
            return fail_html_v2(error, "unsupported named HTML character reference");
        }
        index = semicolon + 1U;
    }
    return true;
}

struct ParsedAttribute {
    explicit ParsedAttribute(std::pmr::memory_resource* memory)
        : name(memory), value(memory) {}

    PmrString name;
    PmrString value;
};

struct ParsedStartTag {
    explicit ParsedStartTag(std::pmr::memory_resource* memory)
        : tag(memory), attributes(memory), role(memory), style(memory) {}

    PmrString tag;
    std::pmr::vector<ParsedAttribute> attributes;
    PmrString role;
    PmrString style;
    bool self_closing{false};
};

void skip_space(std::string_view token, std::size_t* cursor) noexcept {
    while (*cursor < token.size() && ascii_space(token[*cursor])) {
        ++*cursor;
    }
}

bool parse_name(
    std::string_view token,
    std::size_t* cursor,
    bool attribute,
    PmrString* output,
    std::string* error) {
    const std::size_t begin = *cursor;
    while (*cursor < token.size() &&
           (attribute ? attribute_name_character(token[*cursor])
                      : tag_name_character(token[*cursor]))) {
        ++*cursor;
    }
    if (*cursor == begin) {
        return fail_html_v2(
            error,
            attribute ? "missing HTML attribute name" : "missing HTML tag name");
    }
    ascii_lower_assign(token.substr(begin, *cursor - begin), output);
    return true;
}

bool parse_attribute_value(
    std::string_view token,
    std::size_t* cursor,
    PmrString* output,
    std::string* error) {
    if (*cursor >= token.size()) {
        return fail_html_v2(error, "missing HTML attribute value");
    }
    const char first = token[*cursor];
    std::string_view raw;
    if (first == '"' || first == '\'') {
        const char quote = first;
        const std::size_t begin = ++*cursor;
        while (*cursor < token.size() && token[*cursor] != quote) {
            ++*cursor;
        }
        if (*cursor >= token.size()) {
            return fail_html_v2(error, "unterminated quoted HTML attribute value");
        }
        raw = token.substr(begin, *cursor - begin);
        ++*cursor;
    } else {
        const std::size_t begin = *cursor;
        while (*cursor < token.size() && !ascii_space(token[*cursor]) &&
               token[*cursor] != '>' && token[*cursor] != '/') {
            if (token[*cursor] == '<' || token[*cursor] == '"' ||
                token[*cursor] == '\'' || token[*cursor] == '=' ||
                token[*cursor] == '`') {
                return fail_html_v2(
                    error,
                    "invalid byte in unquoted HTML attribute value");
            }
            ++*cursor;
        }
        if (*cursor == begin) {
            return fail_html_v2(error, "empty unquoted HTML attribute value");
        }
        raw = token.substr(begin, *cursor - begin);
    }
    return decode_character_references(raw, output, error);
}

bool parse_start_tag(
    std::string_view token,
    std::uint32_t maximum_attributes,
    std::pmr::memory_resource* memory,
    ParsedStartTag* parsed,
    std::string* error) {
    if (token.size() < 3U || token.front() != '<' || token.back() != '>') {
        return fail_html_v2(error, "invalid HTML start-tag token envelope");
    }
    std::size_t cursor = 1U;
    if (!parse_name(token, &cursor, false, &parsed->tag, error)) {
        return false;
    }
    if (unsupported_special_text_element(view(parsed->tag))) {
        return fail_html_v2(
            error,
            "special HTML tokenizer state is not implemented in strict v2 parser profile: " +
                owned(parsed->tag));
    }
    if (unsupported_foreign_root(view(parsed->tag))) {
        return fail_html_v2(
            error,
            "foreign-content HTML element is not implemented in strict v2 parser profile: " +
                owned(parsed->tag));
    }

    parsed->attributes.clear();
    parsed->role.clear();
    parsed->style.clear();
    parsed->self_closing = false;

    for (;;) {
        skip_space(token, &cursor);
        if (cursor >= token.size()) {
            return fail_html_v2(error, "truncated HTML start tag");
        }
        if (token[cursor] == '>') {
            if (cursor + 1U != token.size()) {
                return fail_html_v2(error, "bytes after HTML start-tag terminator");
            }
            break;
        }
        if (token[cursor] == '/') {
            ++cursor;
            if (cursor >= token.size() || token[cursor] != '>' ||
                cursor + 1U != token.size()) {
                return fail_html_v2(error, "invalid self-closing HTML start tag");
            }
            parsed->self_closing = true;
            break;
        }
        if (parsed->attributes.size() >= maximum_attributes) {
            return fail_html_v2(error, "HTML element exceeds bounded attribute count");
        }

        ParsedAttribute attribute(memory);
        if (!parse_name(token, &cursor, true, &attribute.name, error)) {
            return false;
        }
        for (const ParsedAttribute& existing : parsed->attributes) {
            if (existing.name == attribute.name) {
                return fail_html_v2(
                    error,
                    "duplicate HTML attribute in strict v2 parser profile: " +
                        owned(attribute.name));
            }
        }
        skip_space(token, &cursor);
        if (cursor < token.size() && token[cursor] == '=') {
            ++cursor;
            skip_space(token, &cursor);
            if (!parse_attribute_value(token, &cursor, &attribute.value, error)) {
                return false;
            }
        }
        if (attribute.name == "role") {
            parsed->role = attribute.value;
        } else if (attribute.name == "style") {
            parsed->style = attribute.value;
        }
        parsed->attributes.push_back(std::move(attribute));
    }
    return true;
}

bool parse_end_tag(
    std::string_view token,
    PmrString* tag,
    std::string* error) {
    if (token.size() < 4U || token[0] != '<' || token[1] != '/' ||
        token.back() != '>') {
        return fail_html_v2(error, "invalid HTML end-tag token envelope");
    }
    std::size_t cursor = 2U;
    if (!parse_name(token, &cursor, false, tag, error)) {
        return false;
    }
    skip_space(token, &cursor);
    if (cursor + 1U != token.size() || token[cursor] != '>') {
        return fail_html_v2(
            error,
            "HTML end tag contains unsupported trailing syntax");
    }
    return true;
}

bool parse_doctype(std::string_view token, std::string* error) {
    if (token.size() < 10U || token.front() != '<' || token[1] != '!' ||
        token.back() != '>') {
        return fail_html_v2(error, "invalid HTML declaration");
    }
    std::size_t cursor = 2U;
    const std::size_t word_begin = cursor;
    while (cursor < token.size() && ascii_alpha(token[cursor])) {
        ++cursor;
    }
    if (!ascii_iequals(token.substr(word_begin, cursor - word_begin), "doctype")) {
        return fail_html_v2(error, "unsupported HTML markup declaration");
    }
    skip_space(token, &cursor);
    const std::size_t name_begin = cursor;
    while (cursor < token.size() && ascii_alpha(token[cursor])) {
        ++cursor;
    }
    if (!ascii_iequals(token.substr(name_begin, cursor - name_begin), "html")) {
        return fail_html_v2(error, "strict v2 parser accepts only <!doctype html>");
    }
    skip_space(token, &cursor);
    if (cursor + 1U != token.size() || token[cursor] != '>') {
        return fail_html_v2(
            error,
            "strict v2 parser rejects legacy/public HTML doctypes");
    }
    return true;
}

struct OpenElement {
    OpenElement(
        std::string_view value,
        std::uint64_t node_ordinal,
        std::pmr::memory_resource* memory)
        : tag(value.data(), value.size(), memory), ordinal(node_ordinal) {}

    PmrString tag;
    std::uint64_t ordinal{0U};
};

class StreamingHtmlV2Producer {
public:
    StreamingHtmlV2Producer(
        LogicalNodeSourceV2Writer* writer,
        StreamingHtmlNodeSourceConfig config,
        StreamingHtmlNodeSourceV2Stats* stats,
        std::pmr::memory_resource* memory,
        std::string* error)
        : writer_(writer),
          config_(config),
          stats_(stats),
          memory_(memory),
          error_(error),
          open_elements_(memory),
          token_(memory) {}

    bool begin_document() {
        if (!writer_->append_node(
                LogicalNodeInput{
                    1U,
                    0U,
                    0U,
                    0U,
                    kNoLogicalNodeOrdinal,
                    "#document",
                    "",
                    "",
                    0U},
                {},
                error_)) {
            return false;
        }
        open_elements_.emplace_back("#document", 0U, memory_);
        stats_->nodes_emitted = 1U;
        return true;
    }

    bool feed(
        std::uint64_t record_index,
        std::uint64_t record_offset,
        std::span<const std::byte> bytes) {
        for (std::size_t relative = 0U; relative < bytes.size(); ++relative) {
            if (stats_->source_bytes == std::numeric_limits<std::uint64_t>::max()) {
                return fail_html_v2(error_, "HTML v2 producer source-byte counter overflow");
            }
            if (record_offset >
                std::numeric_limits<std::uint64_t>::max() -
                    static_cast<std::uint64_t>(relative)) {
                return fail_html_v2(error_, "HTML v2 producer record offset overflows");
            }
            ++stats_->source_bytes;
            const char character = static_cast<char>(
                std::to_integer<unsigned char>(bytes[relative]));
            if (!consume_byte(
                    character,
                    record_index,
                    record_offset + static_cast<std::uint64_t>(relative))) {
                return false;
            }
        }
        return true;
    }

    bool finish() {
        if (in_token_) {
            return fail_html_v2(error_, "HTML input ended inside markup token");
        }
        if (!flush_text()) {
            return false;
        }
        if (open_elements_.size() != 1U) {
            return fail_html_v2(
                error_,
                "HTML input ended with unclosed element: " +
                    owned(open_elements_.back().tag));
        }
        return true;
    }

private:
    bool extend_text(
        std::uint64_t record_index,
        std::uint64_t record_offset) {
        if (!text_active_) {
            text_active_ = true;
            text_crossed_record_ = false;
            text_start_record_ = record_index;
            text_start_offset_ = record_offset;
            text_length_ = 0U;
        } else if (record_index != text_start_record_) {
            text_crossed_record_ = true;
        }
        if (text_length_ == std::numeric_limits<std::uint64_t>::max()) {
            return fail_html_v2(error_, "HTML text source span length overflows");
        }
        ++text_length_;
        return true;
    }

    bool append_token_text_prefix(
        std::size_t length,
        bool prefix_crossed_record) {
        if (length == 0U) {
            return true;
        }
        if (length > token_.size()) {
            return fail_html_v2(error_, "HTML text-state candidate prefix exceeds token size");
        }
        if (!text_active_) {
            text_active_ = true;
            text_crossed_record_ = false;
            text_start_record_ = token_start_record_;
            text_start_offset_ = token_start_offset_;
            text_length_ = 0U;
        } else if (token_start_record_ != text_start_record_) {
            text_crossed_record_ = true;
        }
        if (prefix_crossed_record) {
            text_crossed_record_ = true;
        }
        const auto length_u64 = static_cast<std::uint64_t>(length);
        if (text_length_ > std::numeric_limits<std::uint64_t>::max() - length_u64) {
            return fail_html_v2(error_, "HTML text source span length overflows");
        }
        text_length_ += length_u64;
        return true;
    }

    bool flush_text() {
        if (!text_active_) {
            return true;
        }
        if (open_elements_.empty() || text_length_ == 0U) {
            return fail_html_v2(error_, "HTML v2 producer lost text parent state");
        }
        if (writer_->node_count() == std::numeric_limits<std::uint64_t>::max()) {
            return fail_html_v2(error_, "HTML v2 logical-node id overflows");
        }
        const std::uint64_t logical_id = writer_->node_count() + 1U;
        if (!writer_->append_node(
                LogicalNodeInput{
                    logical_id,
                    text_start_record_,
                    text_start_offset_,
                    text_length_,
                    open_elements_.back().ordinal,
                    "#text",
                    "",
                    "",
                    0U},
                {},
                error_)) {
            return false;
        }
        ++stats_->nodes_emitted;
        ++stats_->text_nodes_emitted;
        if (text_crossed_record_) {
            ++stats_->cross_record_text_spans;
        }
        text_active_ = false;
        text_crossed_record_ = false;
        text_length_ = 0U;
        return true;
    }

    bool start_token(
        std::uint64_t record_index,
        std::uint64_t record_offset) {
        in_token_ = true;
        comment_token_ = false;
        quote_ = '\0';
        token_crossed_record_ = false;
        token_start_record_ = record_index;
        token_start_offset_ = record_offset;
        token_.clear();
        token_.push_back('<');
        return true;
    }

    bool append_token_byte(char character, std::uint64_t record_index) {
        if (record_index != token_start_record_) {
            token_crossed_record_ = true;
        }
        if (token_.size() >= config_.maximum_token_bytes) {
            return fail_html_v2(error_, "HTML markup token exceeds bounded byte limit");
        }
        token_.push_back(character);
        return true;
    }

    bool recover_false_text_state_candidate(
        char character,
        std::uint64_t record_index,
        std::uint64_t record_offset,
        bool crossed_before_append) {
        if (character == '<') {
            if (!append_token_text_prefix(token_.size() - 1U, crossed_before_append)) {
                return false;
            }
            return start_token(record_index, record_offset);
        }
        const bool candidate_crossed_record = token_crossed_record_;
        const std::size_t candidate_size = token_.size();
        if (!append_token_text_prefix(candidate_size, candidate_crossed_record)) {
            return false;
        }
        reset_token();
        return true;
    }

    bool consume_text_state_byte(
        char character,
        std::uint64_t record_index,
        std::uint64_t record_offset) {
        if (open_elements_.size() <= 1U) {
            return fail_html_v2(error_, "HTML v2 producer lost active text-state element");
        }
        const std::string_view active_tag = view(open_elements_.back().tag);
        const bool active_raw_text = supported_raw_text_element(active_tag);
        const bool active_rcdata = supported_rcdata_element(active_tag);
        if ((raw_text_ == rcdata_) ||
            (raw_text_ && !active_raw_text) ||
            (rcdata_ && !active_rcdata)) {
            return fail_html_v2(error_, "HTML v2 producer text-state invariant failed");
        }

        if (textarea_initial_lf_pending_) {
            textarea_initial_lf_pending_ = false;
            if (!rcdata_ || active_tag != "textarea") {
                return fail_html_v2(error_, "HTML v2 producer textarea LF state invariant failed");
            }
            if (character == '\n') {
                return true;
            }
            if (character == '\r') {
                return fail_html_v2(
                    error_,
                    "textarea CR/CRLF preprocessing is not implemented in strict v2 parser profile");
            }
        }

        if (!in_token_) {
            if (character == '<') {
                return start_token(record_index, record_offset);
            }
            return extend_text(record_index, record_offset);
        }

        const bool crossed_before_append = token_crossed_record_;
        if (!append_token_byte(character, record_index)) {
            return false;
        }

        const std::size_t closing_size = active_tag.size() + 2U;
        const std::size_t prefix_length = std::min(token_.size(), closing_size);
        for (std::size_t index = 0U; index < prefix_length; ++index) {
            const char expected = index == 0U
                ? '<'
                : (index == 1U ? '/' : active_tag[index - 2U]);
            if (ascii_lower(token_[index]) != expected) {
                return recover_false_text_state_candidate(
                    character,
                    record_index,
                    record_offset,
                    crossed_before_append);
            }
        }
        if (token_.size() <= closing_size) {
            return true;
        }

        const char first_trailing = token_[closing_size];
        if (first_trailing == '>') {
            if (token_.size() != closing_size + 1U) {
                return fail_html_v2(error_, "text-state end tag has bytes after terminator");
            }
            if (!flush_text()) {
                return false;
            }
            const bool result = complete_end_tag();
            if (result) {
                raw_text_ = false;
                rcdata_ = false;
                textarea_initial_lf_pending_ = false;
            }
            reset_token();
            return result;
        }
        if (!ascii_space(first_trailing)) {
            if (first_trailing == '/') {
                return fail_html_v2(
                    error_,
                    "text-state end tag self-closing syntax is unsupported");
            }
            return recover_false_text_state_candidate(
                character,
                record_index,
                record_offset,
                crossed_before_append);
        }

        for (std::size_t index = closing_size + 1U; index < token_.size(); ++index) {
            const char trailing = token_[index];
            if (trailing == '>') {
                if (index + 1U != token_.size()) {
                    return fail_html_v2(
                        error_,
                        "text-state end tag has bytes after terminator");
                }
                if (!flush_text()) {
                    return false;
                }
                const bool result = complete_end_tag();
                if (result) {
                    raw_text_ = false;
                    rcdata_ = false;
                    textarea_initial_lf_pending_ = false;
                }
                reset_token();
                return result;
            }
            if (!ascii_space(trailing)) {
                return fail_html_v2(
                    error_,
                    "text-state end tag contains unsupported trailing syntax");
            }
        }
        return true;
    }

    bool consume_byte(
        char character,
        std::uint64_t record_index,
        std::uint64_t record_offset) {
        if (raw_text_ || rcdata_) {
            return consume_text_state_byte(character, record_index, record_offset);
        }
        if (!in_token_) {
            if (character == '<') {
                if (!flush_text()) {
                    return false;
                }
                return start_token(record_index, record_offset);
            }
            return extend_text(record_index, record_offset);
        }

        if (!append_token_byte(character, record_index)) {
            return false;
        }

        if (comment_token_) {
            if (token_.size() >= 3U &&
                token_[token_.size() - 3U] == '-' &&
                token_[token_.size() - 2U] == '-' &&
                token_.back() == '>') {
                ++stats_->comments_skipped;
                reset_token();
            }
            return true;
        }
        if (token_.size() == 4U && token_ == "<!--") {
            comment_token_ = true;
            return true;
        }

        if (quote_ != '\0') {
            if (character == quote_) {
                quote_ = '\0';
            }
            return true;
        }
        if (character == '"' || character == '\'') {
            quote_ = character;
            return true;
        }
        if (character != '>') {
            return true;
        }

        const bool result = complete_markup_token();
        reset_token();
        return result;
    }

    void reset_token() {
        in_token_ = false;
        comment_token_ = false;
        quote_ = '\0';
        token_crossed_record_ = false;
        token_.clear();
    }

    bool complete_markup_token() {
        if (token_.size() >= 2U && token_[1] == '!') {
            if (!parse_doctype(view(token_), error_)) {
                return false;
            }
            ++stats_->doctypes_skipped;
            return true;
        }
        if (token_.size() >= 2U && token_[1] == '?') {
            return fail_html_v2(
                error_,
                "processing instructions are unsupported in strict v2 HTML profile");
        }
        if (token_.size() >= 2U && token_[1] == '/') {
            return complete_end_tag();
        }
        return complete_start_tag();
    }

    bool complete_end_tag() {
        PmrString tag(memory_);
        if (!parse_end_tag(view(token_), &tag, error_)) {
            return false;
        }
        if (open_elements_.size() <= 1U || open_elements_.back().tag != tag) {
            return fail_html_v2(
                error_,
                "mismatched HTML end tag in strict v2 parser profile: " + owned(tag));
        }
        open_elements_.pop_back();
        return true;
    }

    bool complete_start_tag() {
        ParsedStartTag parsed(memory_);
        if (!parse_start_tag(
                view(token_),
                config_.maximum_attributes_per_element,
                memory_,
                &parsed,
                error_)) {
            return false;
        }
        if (parsed.self_closing && !void_element(view(parsed.tag))) {
            return fail_html_v2(
                error_,
                "self-closing syntax on non-void HTML element is unsupported in strict v2 parser profile: " +
                    owned(parsed.tag));
        }
        if (open_elements_.empty()) {
            return fail_html_v2(error_, "HTML v2 producer lost document root state");
        }
        if (writer_->node_count() == std::numeric_limits<std::uint64_t>::max()) {
            return fail_html_v2(error_, "HTML v2 logical-node id overflows");
        }
        const std::uint64_t logical_id = writer_->node_count() + 1U;
        const std::uint64_t ordinal = logical_id - 1U;
        const std::uint64_t source_length = static_cast<std::uint64_t>(token_.size());

        std::pmr::vector<LogicalNodeAttributeInput> attributes(memory_);
        attributes.reserve(parsed.attributes.size());
        for (const ParsedAttribute& attribute : parsed.attributes) {
            attributes.push_back(LogicalNodeAttributeInput{
                view(attribute.name),
                view(attribute.value),
                0U});
        }
        if (!writer_->append_node(
                LogicalNodeInput{
                    logical_id,
                    token_start_record_,
                    token_start_offset_,
                    source_length,
                    open_elements_.back().ordinal,
                    view(parsed.tag),
                    view(parsed.role),
                    view(parsed.style),
                    0U},
                std::span<const LogicalNodeAttributeInput>(
                    attributes.data(), attributes.size()),
                error_)) {
            return false;
        }

        ++stats_->nodes_emitted;
        ++stats_->element_nodes_emitted;
        stats_->attributes_emitted +=
            static_cast<std::uint64_t>(parsed.attributes.size());
        if (token_crossed_record_) {
            ++stats_->cross_record_markup_spans;
        }

        if (!parsed.self_closing && !void_element(view(parsed.tag))) {
            const std::uint64_t current_depth = open_elements_.size();
            if (current_depth > config_.maximum_open_element_depth) {
                return fail_html_v2(error_, "HTML open-element depth exceeds bounded limit");
            }
            open_elements_.emplace_back(view(parsed.tag), ordinal, memory_);
            if (supported_raw_text_element(view(parsed.tag))) {
                raw_text_ = true;
            } else if (supported_rcdata_element(view(parsed.tag))) {
                rcdata_ = true;
                textarea_initial_lf_pending_ = parsed.tag == "textarea";
            }
            const auto observed = static_cast<std::uint32_t>(
                open_elements_.size() - 1U);
            stats_->maximum_observed_open_depth =
                std::max(stats_->maximum_observed_open_depth, observed);
        }
        return true;
    }

    LogicalNodeSourceV2Writer* writer_{nullptr};
    StreamingHtmlNodeSourceConfig config_{};
    StreamingHtmlNodeSourceV2Stats* stats_{nullptr};
    std::pmr::memory_resource* memory_{nullptr};
    std::string* error_{nullptr};
    std::pmr::vector<OpenElement> open_elements_;
    PmrString token_;

    std::uint64_t token_start_record_{0U};
    std::uint64_t token_start_offset_{0U};
    char quote_{'\0'};
    bool in_token_{false};
    bool comment_token_{false};
    bool token_crossed_record_{false};
    bool raw_text_{false};
    bool rcdata_{false};
    bool textarea_initial_lf_pending_{false};

    std::uint64_t text_start_record_{0U};
    std::uint64_t text_start_offset_{0U};
    std::uint64_t text_length_{0U};
    bool text_active_{false};
    bool text_crossed_record_{false};
};

bool validate_config_v2(
    const StreamingHtmlNodeSourceConfig& config,
    std::string* error) {
    if (config.input_window_bytes == 0U ||
        config.input_window_bytes > kMaximumIoWindowBytes) {
        return fail_html_v2(
            error,
            "HTML v2 producer input window is outside supported bounds");
    }
    if (config.maximum_token_bytes < 4U ||
        config.maximum_token_bytes > kMaximumConfiguredTokenBytes) {
        return fail_html_v2(
            error,
            "HTML v2 producer token bound is outside supported range");
    }
    if (config.maximum_attributes_per_element == 0U ||
        config.maximum_attributes_per_element > kMaximumConfiguredAttributes) {
        return fail_html_v2(
            error,
            "HTML v2 producer attribute bound is outside supported range");
    }
    if (config.maximum_open_element_depth == 0U ||
        config.maximum_open_element_depth > kMaximumConfiguredDepth) {
        return fail_html_v2(
            error,
            "HTML v2 producer depth bound is outside supported range");
    }
    if (config.working_set_limit_bytes == 0U ||
        config.working_set_limit_bytes > kMaximumConfiguredWorkingSetBytes) {
        return fail_html_v2(
            error,
            "HTML v2 producer working-set bound is outside supported range");
    }
    return true;
}

void copy_working_set_stats(
    const zevryon::core::ResourceLedger& ledger,
    StreamingHtmlNodeSourceV2Stats* stats) {
    const zevryon::core::ResourceSnapshot snapshot =
        ledger.snapshot(zevryon::core::ResourceClass::DomProjection);
    stats->working_set_hard_limit_bytes = snapshot.hard_limit_bytes;
    stats->working_set_current_bytes = snapshot.current_bytes;
    stats->working_set_peak_bytes = snapshot.peak_bytes;
    stats->working_set_reservations = snapshot.reservations;
    stats->working_set_releases = snapshot.releases;
    stats->working_set_rejected_reservations = snapshot.rejected_reservations;
    stats->working_set_accounting_errors = snapshot.accounting_errors;
}

bool produce_with_memory(
    const std::filesystem::path& store_root,
    const std::filesystem::path& output_source_path,
    StreamingHtmlNodeSourceConfig config,
    StreamingHtmlNodeSourceV2Stats* stats,
    std::pmr::memory_resource* memory,
    std::string* error) {
    LogicalNodeSourceStoreBinding binding;
    if (!inspect_logical_node_source_store_binding(store_root, &binding, error)) {
        return false;
    }
    if (binding.source_record_count == 0U) {
        return fail_html_v2(error, "HTML v2 producer requires a non-empty native store");
    }

    StoreReadConfig read_config;
    read_config.io_window_bytes = config.input_window_bytes;
    StoreReader store(store_root, read_config);
    if (!store.open(error)) {
        return false;
    }
    stats->source_records = store.stats().corpus.logical_records;

    LogicalNodeSourceV2Writer writer(output_source_path);
    if (!writer.begin(error)) {
        return false;
    }
    {
        StreamingHtmlV2Producer producer(&writer, config, stats, memory, error);
        if (!producer.begin_document()) {
            return false;
        }
        for (std::uint64_t record_index = 0U;
             record_index < store.stats().corpus.logical_records;
             ++record_index) {
            bool parse_ok = true;
            std::uint64_t record_offset = 0U;
            if (!store.read_record(
                    record_index,
                    [&](std::span<const std::byte> chunk) {
                        if (!parse_ok) {
                            return false;
                        }
                        parse_ok = producer.feed(record_index, record_offset, chunk);
                        if (record_offset >
                            std::numeric_limits<std::uint64_t>::max() -
                                static_cast<std::uint64_t>(chunk.size())) {
                            *error = "HTML v2 producer record-offset counter overflows";
                            parse_ok = false;
                            return false;
                        }
                        record_offset += static_cast<std::uint64_t>(chunk.size());
                        return parse_ok;
                    },
                    error)) {
                return false;
            }
            if (!parse_ok) {
                return false;
            }
        }
        if (!producer.finish()) {
            return false;
        }
    }

    if (writer.node_count() != store.stats().corpus.logical_nodes) {
        return fail_html_v2(
            error,
            "HTML v2 parser node count disagrees with native store logical_nodes metadata");
    }
    return writer.finish(binding, error);
}

} // namespace

bool produce_streaming_html_node_source_v2(
    const std::filesystem::path& store_root,
    const std::filesystem::path& output_source_path,
    StreamingHtmlNodeSourceConfig config,
    StreamingHtmlNodeSourceV2Stats* stats,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();

    StreamingHtmlNodeSourceV2Stats local_stats{};
    local_stats.working_set_hard_limit_bytes = config.working_set_limit_bytes;
    if (stats != nullptr) {
        *stats = local_stats;
    }
    if (!validate_config_v2(config, error)) {
        return false;
    }

    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(
        zevryon::core::ResourceClass::DomProjection,
        config.working_set_limit_bytes);

    bool success = false;
    try {
        zevryon::core::LedgerMemoryResource memory(
            ledger,
            zevryon::core::ResourceClass::DomProjection,
            std::pmr::get_default_resource());
        success = produce_with_memory(
            store_root,
            output_source_path,
            config,
            &local_stats,
            &memory,
            error);
    } catch (const std::bad_alloc&) {
        const zevryon::core::ResourceSnapshot snapshot =
            ledger.snapshot(zevryon::core::ResourceClass::DomProjection);
        if (snapshot.rejected_reservations != 0U) {
            fail_html_v2(error, "HTML v2 parser working-set hard limit exhausted");
        } else {
            fail_html_v2(error, "HTML v2 parser allocation failed");
        }
        success = false;
    }

    copy_working_set_stats(ledger, &local_stats);
    if (local_stats.working_set_current_bytes != 0U ||
        local_stats.working_set_accounting_errors != 0U) {
        fail_html_v2(
            error,
            "HTML v2 parser working-set accounting did not release cleanly");
        success = false;
    }
    if (stats != nullptr) {
        *stats = local_stats;
    }
    return success;
}

} // namespace zevryon::massivedoc
