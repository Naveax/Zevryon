#include "streaming_html_node_source.hpp"

#include "logical_node_source.hpp"
#include "massivedoc_store.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zevryon::massivedoc {
namespace {

constexpr std::size_t kMaximumConfiguredTokenBytes = 16U * 1024U * 1024U;
constexpr std::uint32_t kMaximumConfiguredAttributes = 65536U;
constexpr std::uint32_t kMaximumConfiguredDepth = 65536U;

bool fail_html(std::string* error, std::string message) {
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

std::string ascii_lower_copy(std::string_view value) {
    std::string output;
    output.reserve(value.size());
    for (const char character : value) {
        output.push_back(ascii_lower(character));
    }
    return output;
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

bool unsupported_raw_text_element(std::string_view tag) noexcept {
    constexpr std::string_view values[] = {
        "script", "style", "title", "textarea", "xmp", "iframe",
        "noembed", "noframes", "plaintext"};
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

bool append_utf8(std::uint32_t codepoint, std::string* output) {
    if (codepoint == 0U || codepoint > 0x10ffffU ||
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
    std::string* output,
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
            return fail_html(error, "unterminated or oversized HTML character reference");
        }
        const std::string_view entity = input.substr(index + 1U, semicolon - index - 1U);
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
                return fail_html(error, "empty numeric HTML character reference");
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
                    return fail_html(error, "invalid numeric HTML character reference");
                }
                if (digit >= base || codepoint > (0x10ffffU - digit) / base) {
                    return fail_html(error, "HTML character reference overflows Unicode range");
                }
                codepoint = codepoint * base + digit;
            }
            if (!append_utf8(codepoint, output)) {
                return fail_html(error, "invalid Unicode scalar in HTML character reference");
            }
        } else {
            return fail_html(error, "unsupported named HTML character reference");
        }
        index = semicolon + 1U;
    }
    return true;
}

struct ParsedAttribute {
    std::string name;
    std::string value;
};

struct ParsedStartTag {
    std::string tag;
    std::vector<ParsedAttribute> attributes;
    std::string role;
    std::string style;
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
    std::string* output,
    std::string* error) {
    const std::size_t begin = *cursor;
    while (*cursor < token.size() &&
           (attribute ? attribute_name_character(token[*cursor])
                      : tag_name_character(token[*cursor]))) {
        ++*cursor;
    }
    if (*cursor == begin) {
        return fail_html(error, attribute ? "missing HTML attribute name" : "missing HTML tag name");
    }
    *output = ascii_lower_copy(token.substr(begin, *cursor - begin));
    return true;
}

bool parse_attribute_value(
    std::string_view token,
    std::size_t* cursor,
    std::string* output,
    std::string* error) {
    if (*cursor >= token.size()) {
        return fail_html(error, "missing HTML attribute value");
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
            return fail_html(error, "unterminated quoted HTML attribute value");
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
                return fail_html(error, "invalid byte in unquoted HTML attribute value");
            }
            ++*cursor;
        }
        if (*cursor == begin) {
            return fail_html(error, "empty unquoted HTML attribute value");
        }
        raw = token.substr(begin, *cursor - begin);
    }
    return decode_character_references(raw, output, error);
}

bool parse_start_tag(
    std::string_view token,
    std::uint32_t maximum_attributes,
    ParsedStartTag* parsed,
    std::string* error) {
    if (token.size() < 3U || token.front() != '<' || token.back() != '>') {
        return fail_html(error, "invalid HTML start-tag token envelope");
    }
    std::size_t cursor = 1U;
    if (!parse_name(token, &cursor, false, &parsed->tag, error)) {
        return false;
    }
    if (unsupported_raw_text_element(parsed->tag)) {
        return fail_html(error, "raw-text HTML element is not implemented in strict parser profile: " + parsed->tag);
    }
    if (unsupported_foreign_root(parsed->tag)) {
        return fail_html(error, "foreign-content HTML element is not implemented in strict parser profile: " + parsed->tag);
    }

    parsed->attributes.clear();
    parsed->role.clear();
    parsed->style.clear();
    parsed->self_closing = false;

    for (;;) {
        skip_space(token, &cursor);
        if (cursor >= token.size()) {
            return fail_html(error, "truncated HTML start tag");
        }
        if (token[cursor] == '>') {
            if (cursor + 1U != token.size()) {
                return fail_html(error, "bytes after HTML start-tag terminator");
            }
            break;
        }
        if (token[cursor] == '/') {
            ++cursor;
            if (cursor >= token.size() || token[cursor] != '>' ||
                cursor + 1U != token.size()) {
                return fail_html(error, "invalid self-closing HTML start tag");
            }
            parsed->self_closing = true;
            break;
        }
        if (parsed->attributes.size() >= maximum_attributes) {
            return fail_html(error, "HTML element exceeds bounded attribute count");
        }

        ParsedAttribute attribute;
        if (!parse_name(token, &cursor, true, &attribute.name, error)) {
            return false;
        }
        for (const ParsedAttribute& existing : parsed->attributes) {
            if (existing.name == attribute.name) {
                return fail_html(error, "duplicate HTML attribute in strict parser profile: " + attribute.name);
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
    std::string* tag,
    std::string* error) {
    if (token.size() < 4U || token[0] != '<' || token[1] != '/' ||
        token.back() != '>') {
        return fail_html(error, "invalid HTML end-tag token envelope");
    }
    std::size_t cursor = 2U;
    if (!parse_name(token, &cursor, false, tag, error)) {
        return false;
    }
    skip_space(token, &cursor);
    if (cursor + 1U != token.size() || token[cursor] != '>') {
        return fail_html(error, "HTML end tag contains unsupported trailing syntax");
    }
    return true;
}

bool parse_doctype(std::string_view token, std::string* error) {
    if (token.size() < 10U || token.front() != '<' || token[1] != '!' ||
        token.back() != '>') {
        return fail_html(error, "invalid HTML declaration");
    }
    std::size_t cursor = 2U;
    const std::size_t word_begin = cursor;
    while (cursor < token.size() && ascii_alpha(token[cursor])) {
        ++cursor;
    }
    if (!ascii_iequals(token.substr(word_begin, cursor - word_begin), "doctype")) {
        return fail_html(error, "unsupported HTML markup declaration");
    }
    skip_space(token, &cursor);
    const std::size_t name_begin = cursor;
    while (cursor < token.size() && ascii_alpha(token[cursor])) {
        ++cursor;
    }
    if (!ascii_iequals(token.substr(name_begin, cursor - name_begin), "html")) {
        return fail_html(error, "strict parser profile accepts only <!doctype html>");
    }
    skip_space(token, &cursor);
    if (cursor + 1U != token.size() || token[cursor] != '>') {
        return fail_html(error, "strict parser profile rejects legacy/public HTML doctypes");
    }
    return true;
}

struct OpenElement {
    std::string tag;
    std::uint64_t ordinal{0U};
};

class StreamingHtmlProducer {
public:
    StreamingHtmlProducer(
        LogicalNodeSourceWriter* writer,
        StreamingHtmlNodeSourceConfig config,
        StreamingHtmlNodeSourceStats* stats,
        std::string* error)
        : writer_(writer), config_(config), stats_(stats), error_(error) {
        open_elements_.reserve(
            static_cast<std::size_t>(config_.maximum_open_element_depth) + 1U);
    }

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
        open_elements_.push_back(OpenElement{"#document", 0U});
        stats_->nodes_emitted = 1U;
        stats_->maximum_observed_open_depth = 0U;
        return true;
    }

    bool feed(
        std::uint64_t record_index,
        std::uint64_t record_offset,
        std::span<const std::byte> bytes) {
        for (std::size_t relative = 0U; relative < bytes.size(); ++relative) {
            if (stats_->source_bytes == std::numeric_limits<std::uint64_t>::max()) {
                return fail_html(error_, "HTML producer source-byte counter overflow");
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
            return fail_html(error_, "HTML input ended inside markup token");
        }
        if (open_elements_.size() != 1U) {
            return fail_html(
                error_,
                "HTML input ended with unclosed element: " + open_elements_.back().tag);
        }
        return true;
    }

private:
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
            return fail_html(error_, "HTML markup token exceeds bounded byte limit");
        }
        token_.push_back(character);
        return true;
    }

    bool consume_byte(
        char character,
        std::uint64_t record_index,
        std::uint64_t record_offset) {
        if (!in_token_) {
            if (character == '<') {
                return start_token(record_index, record_offset);
            }
            return true;
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
            if (!parse_doctype(token_, error_)) {
                return false;
            }
            ++stats_->doctypes_skipped;
            return true;
        }
        if (token_.size() >= 2U && token_[1] == '?') {
            return fail_html(error_, "processing instructions are unsupported in strict HTML profile");
        }
        if (token_.size() >= 2U && token_[1] == '/') {
            return complete_end_tag();
        }
        return complete_start_tag();
    }

    bool complete_end_tag() {
        std::string tag;
        if (!parse_end_tag(token_, &tag, error_)) {
            return false;
        }
        if (open_elements_.size() <= 1U || open_elements_.back().tag != tag) {
            return fail_html(error_, "mismatched HTML end tag in strict parser profile: " + tag);
        }
        open_elements_.pop_back();
        return true;
    }

    bool complete_start_tag() {
        ParsedStartTag parsed;
        if (!parse_start_tag(
                token_,
                config_.maximum_attributes_per_element,
                &parsed,
                error_)) {
            return false;
        }
        if (open_elements_.empty()) {
            return fail_html(error_, "HTML producer lost document root state");
        }
        const std::uint64_t logical_id = writer_->node_count() + 1U;
        if (logical_id == 0U) {
            return fail_html(error_, "HTML logical-node id overflow");
        }
        const std::uint64_t ordinal = logical_id - 1U;
        const std::uint64_t source_length = token_crossed_record_
            ? 0U
            : static_cast<std::uint64_t>(token_.size());

        std::vector<LogicalNodeAttributeInput> attributes;
        attributes.reserve(parsed.attributes.size());
        for (const ParsedAttribute& attribute : parsed.attributes) {
            attributes.push_back(LogicalNodeAttributeInput{
                attribute.name,
                attribute.value,
                0U});
        }
        if (!writer_->append_node(
                LogicalNodeInput{
                    logical_id,
                    token_start_record_,
                    token_start_offset_,
                    source_length,
                    open_elements_.back().ordinal,
                    parsed.tag,
                    parsed.role,
                    parsed.style,
                    0U},
                attributes,
                error_)) {
            return false;
        }

        ++stats_->nodes_emitted;
        stats_->attributes_emitted +=
            static_cast<std::uint64_t>(parsed.attributes.size());
        if (token_crossed_record_) {
            ++stats_->cross_record_token_anchors;
        }

        if (!parsed.self_closing && !void_element(parsed.tag)) {
            const std::uint64_t current_depth = open_elements_.size();
            if (current_depth > config_.maximum_open_element_depth) {
                return fail_html(error_, "HTML open-element depth exceeds bounded limit");
            }
            open_elements_.push_back(OpenElement{parsed.tag, ordinal});
            const auto observed = static_cast<std::uint32_t>(open_elements_.size() - 1U);
            stats_->maximum_observed_open_depth =
                std::max(stats_->maximum_observed_open_depth, observed);
        }
        return true;
    }

    LogicalNodeSourceWriter* writer_{nullptr};
    StreamingHtmlNodeSourceConfig config_{};
    StreamingHtmlNodeSourceStats* stats_{nullptr};
    std::string* error_{nullptr};
    std::vector<OpenElement> open_elements_;
    std::string token_;
    std::uint64_t token_start_record_{0U};
    std::uint64_t token_start_offset_{0U};
    char quote_{'\0'};
    bool in_token_{false};
    bool comment_token_{false};
    bool token_crossed_record_{false};
};

bool validate_config(
    const StreamingHtmlNodeSourceConfig& config,
    std::string* error) {
    if (config.input_window_bytes == 0U ||
        config.input_window_bytes > kMaximumIoWindowBytes) {
        return fail_html(error, "HTML producer input window is outside supported bounds");
    }
    if (config.maximum_token_bytes < 4U ||
        config.maximum_token_bytes > kMaximumConfiguredTokenBytes) {
        return fail_html(error, "HTML producer token bound is outside supported range");
    }
    if (config.maximum_attributes_per_element == 0U ||
        config.maximum_attributes_per_element > kMaximumConfiguredAttributes) {
        return fail_html(error, "HTML producer attribute bound is outside supported range");
    }
    if (config.maximum_open_element_depth == 0U ||
        config.maximum_open_element_depth > kMaximumConfiguredDepth) {
        return fail_html(error, "HTML producer depth bound is outside supported range");
    }
    return true;
}

} // namespace

bool produce_streaming_html_node_source(
    const std::filesystem::path& store_root,
    const std::filesystem::path& output_source_path,
    StreamingHtmlNodeSourceConfig config,
    StreamingHtmlNodeSourceStats* stats,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (!validate_config(config, error)) {
        return false;
    }

    StreamingHtmlNodeSourceStats local_stats{};
    LogicalNodeSourceStoreBinding binding;
    if (!inspect_logical_node_source_store_binding(store_root, &binding, error)) {
        return false;
    }
    if (binding.source_record_count == 0U) {
        return fail_html(error, "HTML producer requires a non-empty native store");
    }

    StoreReadConfig read_config;
    read_config.io_window_bytes = config.input_window_bytes;
    StoreReader store(store_root, read_config);
    if (!store.open(error)) {
        return false;
    }
    local_stats.source_records = store.stats().corpus.logical_records;

    LogicalNodeSourceWriter writer(output_source_path);
    if (!writer.begin(error)) {
        return false;
    }
    StreamingHtmlProducer producer(&writer, config, &local_stats, error);
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
    if (writer.node_count() != store.stats().corpus.logical_nodes) {
        return fail_html(
            error,
            "HTML parser node count disagrees with native store logical_nodes metadata");
    }
    if (!writer.finish(binding, error)) {
        return false;
    }
    if (stats != nullptr) {
        *stats = local_stats;
    }
    return true;
}

} // namespace zevryon::massivedoc
