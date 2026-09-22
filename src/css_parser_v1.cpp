#include "css_parser_v1.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

namespace zevryon::style {
namespace {

constexpr std::size_t kMaximumConfiguredInputBytes = 16U * 1024U * 1024U;
constexpr std::uint32_t kMaximumConfiguredRules = 1'048'576U;
constexpr std::uint32_t kMaximumConfiguredDeclarations = 4'194'304U;
constexpr std::uint32_t kMaximumConfiguredAtRules = 1'048'576U;
constexpr std::uint32_t kMaximumConfiguredNestingDepth = 256U;
constexpr std::size_t kMaximumConfiguredOutputTextBytes = 128U * 1024U * 1024U;

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
        const int lowered = static_cast<int>(value) +
            (static_cast<int>('a') - static_cast<int>('A'));
        return static_cast<char>(lowered);
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

std::string_view trim_ascii(std::string_view value) noexcept {
    std::size_t first = 0U;
    while (first < value.size() && ascii_space(value[first])) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && ascii_space(value[last - 1U])) {
        --last;
    }
    return value.substr(first, last - first);
}

bool set_error(
    CssParserV1Error* error,
    CssParserV1ErrorKind kind,
    std::size_t byte_offset,
    const char* message) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->byte_offset = byte_offset;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool continuation_byte(unsigned char value) noexcept {
    return (value & 0xc0U) == 0x80U;
}

void append_replacement_character(std::pmr::string* output) {
    output->append("\xef\xbf\xbd", 3U);
}

void preprocess_css_input(
    std::string_view input,
    std::pmr::string* output,
    CssParserV1Stats* stats) {
    output->clear();
    output->reserve(input.size());

    std::size_t index = 0U;
    while (index < input.size()) {
        const unsigned char first =
            static_cast<unsigned char>(input[index]);

        if (first == 0U) {
            append_replacement_character(output);
            ++stats->null_replacements;
            ++index;
            continue;
        }
        if (first == static_cast<unsigned char>('\r')) {
            output->push_back('\n');
            ++stats->newline_normalizations;
            ++index;
            if (index < input.size() && input[index] == '\n') {
                ++index;
            }
            continue;
        }
        if (first == static_cast<unsigned char>('\f')) {
            output->push_back('\n');
            ++stats->newline_normalizations;
            ++index;
            continue;
        }
        if (first < 0x80U) {
            output->push_back(static_cast<char>(first));
            ++index;
            continue;
        }

        std::size_t sequence_length = 0U;
        std::uint32_t codepoint = 0U;
        if (first >= 0xc2U && first <= 0xdfU) {
            sequence_length = 2U;
            codepoint = static_cast<std::uint32_t>(first & 0x1fU);
        } else if (first >= 0xe0U && first <= 0xefU) {
            sequence_length = 3U;
            codepoint = static_cast<std::uint32_t>(first & 0x0fU);
        } else if (first >= 0xf0U && first <= 0xf4U) {
            sequence_length = 4U;
            codepoint = static_cast<std::uint32_t>(first & 0x07U);
        } else {
            append_replacement_character(output);
            ++stats->invalid_utf8_replacements;
            ++index;
            continue;
        }

        if (sequence_length > input.size() - index) {
            append_replacement_character(output);
            ++stats->invalid_utf8_replacements;
            ++index;
            continue;
        }

        bool continuations_valid = true;
        for (std::size_t offset = 1U; offset < sequence_length; ++offset) {
            const unsigned char next =
                static_cast<unsigned char>(input[index + offset]);
            if (!continuation_byte(next)) {
                continuations_valid = false;
                break;
            }
            codepoint =
                (codepoint << 6U) |
                static_cast<std::uint32_t>(next & 0x3fU);
        }
        if (!continuations_valid) {
            append_replacement_character(output);
            ++stats->invalid_utf8_replacements;
            ++index;
            continue;
        }

        const bool overlong =
            (sequence_length == 2U && codepoint < 0x80U) ||
            (sequence_length == 3U && codepoint < 0x800U) ||
            (sequence_length == 4U && codepoint < 0x10000U);
        const bool surrogate =
            codepoint >= 0xd800U && codepoint <= 0xdfffU;
        const bool out_of_range = codepoint > 0x10ffffU;
        if (overlong || surrogate || out_of_range) {
            append_replacement_character(output);
            ++stats->invalid_utf8_replacements;
            index += sequence_length;
            continue;
        }

        output->append(input.data() + index, sequence_length);
        index += sequence_length;
    }

    stats->preprocessed_input_bytes =
        static_cast<std::uint64_t>(output->size());
}

bool valid_property_name(std::string_view property) noexcept {
    if (property.empty()) {
        return false;
    }
    const bool custom = property.size() > 2U && property[0] == '-' && property[1] == '-';
    std::size_t index = custom ? 2U : 0U;
    if (!custom) {
        const char first = property[0];
        if (!(ascii_alpha(first) || first == '_' || first == '-')) {
            return false;
        }
        index = 1U;
    }
    for (; index < property.size(); ++index) {
        const char value = property[index];
        if (!(ascii_alpha(value) || ascii_digit(value) || value == '_' || value == '-')) {
            return false;
        }
    }
    return true;
}

struct ImportantSplit {
    std::string_view value;
    bool important{false};
};

ImportantSplit split_important(std::string_view value) noexcept {
    value = trim_ascii(value);
    constexpr std::string_view keyword = "important";
    if (value.size() < keyword.size()) {
        return ImportantSplit{value, false};
    }
    const std::size_t keyword_offset = value.size() - keyword.size();
    if (!ascii_iequals(value.substr(keyword_offset), keyword)) {
        return ImportantSplit{value, false};
    }
    std::size_t cursor = keyword_offset;
    while (cursor > 0U && ascii_space(value[cursor - 1U])) {
        --cursor;
    }
    if (cursor == 0U || value[cursor - 1U] != '!') {
        return ImportantSplit{value, false};
    }
    const std::string_view stripped = trim_ascii(value.substr(0U, cursor - 1U));
    return ImportantSplit{stripped, true};
}

class Parser final {
public:
    Parser(
        std::string_view input,
        CssParserV1Config config,
        CssStylesheetV1* output,
        CssParserV1Stats* stats,
        CssParserV1Error* error)
        : input_(input), config_(config), output_(output), stats_(stats), error_(error) {}

    bool run() {
        while (true) {
            if (!skip_trivia()) {
                return false;
            }
            if (cursor_ == input_.size()) {
                stats_->output_text_bytes = static_cast<std::uint64_t>(output_->text.size());
                return true;
            }
            if (input_[cursor_] == '}') {
                return fail(CssParserV1ErrorKind::UnbalancedBlock, cursor_, "unexpected top-level CSS closing brace");
            }
            if (input_[cursor_] == '@') {
                if (!parse_at_rule(
                        CssAtRuleContextV1::TopLevel,
                        kCssAtRuleNoOwnerV1)) {
                    return false;
                }
                continue;
            }
            if (!parse_style_rule()) {
                return false;
            }
        }
    }

    bool run_declaration_list() {
        bool closed_list = false;
        while (!closed_list) {
            if (!skip_trivia()) {
                return false;
            }
            if (cursor_ == input_.size()) {
                return fail(
                    CssParserV1ErrorKind::UnbalancedBlock,
                    cursor_,
                    "standalone CSS declaration list lost its internal close sentinel");
            }
            if (input_[cursor_] == '}') {
                ++cursor_;
                closed_list = true;
                break;
            }
            if (input_[cursor_] == ';') {
                ++cursor_;
                continue;
            }
            if (input_[cursor_] == '@') {
                if (!parse_at_rule(
                        CssAtRuleContextV1::DeclarationList,
                        kCssAtRuleNoOwnerV1)) {
                    return false;
                }
                continue;
            }
            if (!parse_declaration(&closed_list)) {
                if (error_->kind !=
                    CssParserV1ErrorKind::InvalidDeclaration) {
                    return false;
                }
                ++stats_->recovered_invalid_declarations;
                clear_error_after_recovery();
                if (!recover_bad_declaration(&closed_list)) {
                    return false;
                }
            }
        }

        if (cursor_ != input_.size()) {
            return fail(
                CssParserV1ErrorKind::UnbalancedBlock,
                cursor_ == 0U ? 0U : cursor_ - 1U,
                "standalone CSS declaration list contains an unexpected closing brace");
        }
        stats_->output_text_bytes =
            static_cast<std::uint64_t>(output_->text.size());
        return true;
    }


private:
    bool fail(CssParserV1ErrorKind kind, std::size_t offset, const char* message) noexcept {
        return set_error(error_, kind, offset, message);
    }

    bool skip_comment() {
        const std::size_t start = cursor_;
        cursor_ += 2U;
        while (cursor_ + 1U < input_.size()) {
            if (input_[cursor_] == '*' && input_[cursor_ + 1U] == '/') {
                cursor_ += 2U;
                ++stats_->comments;
                return true;
            }
            ++cursor_;
        }
        return fail(CssParserV1ErrorKind::UnterminatedComment, start, "unterminated CSS comment");
    }

    bool skip_trivia() {
        while (cursor_ < input_.size()) {
            if (ascii_space(input_[cursor_])) {
                ++cursor_;
                continue;
            }
            if (cursor_ + 1U < input_.size() && input_[cursor_] == '/' && input_[cursor_ + 1U] == '*') {
                if (!skip_comment()) {
                    return false;
                }
                continue;
            }
            break;
        }
        return true;
    }

    bool push_nesting(char closer, std::size_t offset, std::array<char, kMaximumConfiguredNestingDepth>* stack, std::uint32_t* depth) {
        if (*depth >= config_.maximum_nesting_depth) {
            return fail(CssParserV1ErrorKind::NestingLimitExceeded, offset, "CSS nesting depth exceeds configured limit");
        }
        (*stack)[*depth] = closer;
        ++(*depth);
        if (*depth > stats_->maximum_nesting_depth) {
            stats_->maximum_nesting_depth = *depth;
        }
        return true;
    }

    bool consume_string(char quote) {
        const std::size_t start = cursor_;
        ++cursor_;
        while (cursor_ < input_.size()) {
            const char value = input_[cursor_];
            if (value == quote) {
                ++cursor_;
                return true;
            }
            if (value == '\\') {
                ++cursor_;
                if (cursor_ == input_.size()) {
                    return fail(CssParserV1ErrorKind::UnterminatedString, start, "unterminated CSS escape in string");
                }
                ++cursor_;
                continue;
            }
            if (value == '\n' || value == '\r' || value == '\f') {
                return fail(CssParserV1ErrorKind::UnterminatedString, start, "unescaped newline in CSS string");
            }
            ++cursor_;
        }
        return fail(CssParserV1ErrorKind::UnterminatedString, start, "unterminated CSS string");
    }

    bool append_slice(std::string_view value, bool lowercase, CssTextSliceV1* slice) {
        if (slice == nullptr) {
            return fail(CssParserV1ErrorKind::InvalidConfiguration, cursor_, "CSS output slice is null");
        }
        if (value.size() > config_.maximum_output_text_bytes - output_->text.size()) {
            return fail(CssParserV1ErrorKind::OutputBudgetExceeded, cursor_, "CSS output text budget exceeded");
        }
        if (output_->text.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
            value.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            return fail(CssParserV1ErrorKind::OutputBudgetExceeded, cursor_, "CSS output text offset exceeds 32-bit representation");
        }
        slice->offset = static_cast<std::uint32_t>(output_->text.size());
        slice->length = static_cast<std::uint32_t>(value.size());
        if (lowercase) {
            for (const char character : value) {
                output_->text.push_back(ascii_lower(character));
            }
        } else {
            output_->text.append(value.data(), value.size());
        }
        return true;
    }

    void clear_error_after_recovery() noexcept {
        error_->kind = CssParserV1ErrorKind::None;
        error_->byte_offset = 0U;
        error_->message.clear();
    }

    bool parse_at_rule_name(std::string_view* name) {
        if (name == nullptr || cursor_ >= input_.size()) {
            return fail(
                CssParserV1ErrorKind::InvalidAtRule,
                cursor_,
                "CSS at-rule name is missing");
        }

        const std::size_t start = cursor_;
        const auto name_start_byte = [&](std::size_t index) noexcept {
            if (index >= input_.size()) {
                return false;
            }
            const char value = input_[index];
            const unsigned char byte =
                static_cast<unsigned char>(value);
            return ascii_alpha(value) || value == '_' ||
                byte >= 0x80U;
        };

        const char first = input_[cursor_];
        if (first == '\\') {
            return fail(
                CssParserV1ErrorKind::UnsupportedSyntax,
                cursor_,
                "escaped CSS at-rule names are outside this foundation");
        }
        if (first == '-') {
            if (cursor_ + 1U >= input_.size()) {
                return fail(
                    CssParserV1ErrorKind::InvalidAtRule,
                    cursor_,
                    "CSS at-rule hyphen name is incomplete");
            }
            const char second = input_[cursor_ + 1U];
            if (second == '\\') {
                return fail(
                    CssParserV1ErrorKind::UnsupportedSyntax,
                    cursor_ + 1U,
                    "escaped CSS at-rule names are outside this foundation");
            }
            if (!(second == '-' ||
                  name_start_byte(cursor_ + 1U))) {
                return fail(
                    CssParserV1ErrorKind::InvalidAtRule,
                    cursor_,
                    "CSS at-rule hyphen name does not start an identifier");
            }
        } else if (!name_start_byte(cursor_)) {
            return fail(
                CssParserV1ErrorKind::InvalidAtRule,
                cursor_,
                "CSS at-rule name has invalid initial byte");
        }

        while (cursor_ < input_.size()) {
            const char value = input_[cursor_];
            const unsigned char byte =
                static_cast<unsigned char>(value);
            if (ascii_alpha(value) || ascii_digit(value) ||
                value == '_' || value == '-' || byte >= 0x80U) {
                ++cursor_;
                continue;
            }
            if (value == '\\') {
                return fail(
                    CssParserV1ErrorKind::UnsupportedSyntax,
                    cursor_,
                    "escaped CSS at-rule names are outside this foundation");
            }
            break;
        }

        *name = input_.substr(start, cursor_ - start);
        return !name->empty();
    }

    bool consume_at_rule_block(std::string_view* block) {
        if (block == nullptr) {
            return fail(
                CssParserV1ErrorKind::InvalidConfiguration,
                cursor_,
                "CSS at-rule block output is null");
        }
        const std::size_t start = cursor_;
        std::array<char, kMaximumConfiguredNestingDepth> stack{};
        std::uint32_t depth = 0U;
        if (!push_nesting(
                '}',
                start == 0U ? 0U : start - 1U,
                &stack,
                &depth)) {
            return false;
        }

        while (cursor_ < input_.size()) {
            const char value = input_[cursor_];
            if (value == '"' || value == '\'') {
                if (!consume_string(value)) {
                    return false;
                }
                continue;
            }
            if (cursor_ + 1U < input_.size() &&
                value == '/' && input_[cursor_ + 1U] == '*') {
                if (!skip_comment()) {
                    return false;
                }
                continue;
            }
            if (value == '\\') {
                ++cursor_;
                if (cursor_ == input_.size()) {
                    return fail(
                        CssParserV1ErrorKind::InvalidAtRule,
                        start,
                        "CSS at-rule block has dangling escape");
                }
                ++cursor_;
                continue;
            }
            if (value == '(' || value == '[' || value == '{') {
                const char closer =
                    value == '(' ? ')' :
                    (value == '[' ? ']' : '}');
                if (!push_nesting(
                        closer,
                        cursor_,
                        &stack,
                        &depth)) {
                    return false;
                }
                ++cursor_;
                continue;
            }
            if (value == ')' || value == ']' || value == '}') {
                if (depth == 0U || stack[depth - 1U] != value) {
                    return fail(
                        CssParserV1ErrorKind::InvalidAtRule,
                        cursor_,
                        "mismatched CSS at-rule block delimiter");
                }
                --depth;
                if (depth == 0U) {
                    *block = input_.substr(start, cursor_ - start);
                    ++cursor_;
                    return true;
                }
                ++cursor_;
                continue;
            }
            ++cursor_;
        }
        return fail(
            CssParserV1ErrorKind::UnbalancedBlock,
            start,
            "CSS at-rule block is unterminated");
    }

    bool emit_at_rule(
        std::string_view name,
        std::string_view prelude,
        std::string_view block,
        bool has_block,
        CssAtRuleContextV1 context,
        std::uint32_t owner_rule_index,
        std::size_t start) {
        if (context == CssAtRuleContextV1::TopLevel &&
            ascii_iequals(name, "charset")) {
            ++stats_->dropped_charset_rules;
            return true;
        }
        if (output_->at_rules.size() >= config_.maximum_at_rules) {
            return fail(
                CssParserV1ErrorKind::AtRuleLimitExceeded,
                start,
                "CSS at-rule count exceeds configured limit");
        }

        CssAtRuleV1 rule;
        if (!append_slice(name, true, &rule.name) ||
            !append_slice(prelude, false, &rule.prelude)) {
            return false;
        }
        if (has_block &&
            !append_slice(block, false, &rule.block)) {
            return false;
        }
        rule.context = context;
        rule.owner_rule_index = owner_rule_index;
        rule.has_block = has_block;
        output_->at_rules.push_back(rule);
        ++stats_->at_rules;
        return true;
    }

    bool parse_at_rule(
        CssAtRuleContextV1 context,
        std::uint32_t owner_rule_index) {
        const std::size_t start = cursor_;
        if (input_[cursor_] != '@') {
            return fail(
                CssParserV1ErrorKind::InvalidAtRule,
                cursor_,
                "CSS at-rule parser did not start at @");
        }
        ++cursor_;
        std::string_view name;
        if (!parse_at_rule_name(&name)) {
            return false;
        }

        const std::size_t prelude_start = cursor_;
        std::array<char, kMaximumConfiguredNestingDepth> stack{};
        std::uint32_t depth = 0U;
        while (cursor_ < input_.size()) {
            const char value = input_[cursor_];
            if (value == '"' || value == '\'') {
                if (!consume_string(value)) {
                    return false;
                }
                continue;
            }
            if (cursor_ + 1U < input_.size() &&
                value == '/' && input_[cursor_ + 1U] == '*') {
                if (!skip_comment()) {
                    return false;
                }
                continue;
            }
            if (value == '\\') {
                ++cursor_;
                if (cursor_ == input_.size()) {
                    return fail(
                        CssParserV1ErrorKind::InvalidAtRule,
                        start,
                        "CSS at-rule prelude has dangling escape");
                }
                ++cursor_;
                continue;
            }
            if (value == '(' || value == '[' ||
                (value == '{' && depth != 0U)) {
                const char closer =
                    value == '(' ? ')' : (value == '[' ? ']' : '}');
                if (!push_nesting(
                        closer,
                        cursor_,
                        &stack,
                        &depth)) {
                    return false;
                }
                ++cursor_;
                continue;
            }
            if (value == ')' || value == ']' ||
                (value == '}' && depth != 0U)) {
                if (depth == 0U || stack[depth - 1U] != value) {
                    return fail(
                        CssParserV1ErrorKind::InvalidAtRule,
                        cursor_,
                        "mismatched CSS at-rule prelude delimiter");
                }
                --depth;
                ++cursor_;
                continue;
            }
            if (value == ';' && depth == 0U) {
                const std::string_view prelude =
                    trim_ascii(input_.substr(
                        prelude_start,
                        cursor_ - prelude_start));
                ++cursor_;
                return emit_at_rule(
                    name,
                    prelude,
                    {},
                    false,
                    context,
                    owner_rule_index,
                    start);
            }
            if (value == '{' && depth == 0U) {
                const std::string_view prelude =
                    trim_ascii(input_.substr(
                        prelude_start,
                        cursor_ - prelude_start));
                ++cursor_;
                std::string_view block;
                if (!consume_at_rule_block(&block)) {
                    return false;
                }
                return emit_at_rule(
                    name,
                    prelude,
                    block,
                    true,
                    context,
                    owner_rule_index,
                    start);
            }
            if (value == '}' && depth == 0U) {
                return fail(
                    CssParserV1ErrorKind::InvalidAtRule,
                    cursor_,
                    "CSS at-rule prelude closed by declaration block");
            }
            ++cursor_;
        }

        if (depth != 0U) {
            return fail(
                CssParserV1ErrorKind::UnbalancedBlock,
                start,
                "CSS at-rule prelude is unterminated");
        }
        const std::string_view prelude =
            trim_ascii(input_.substr(
                prelude_start,
                cursor_ - prelude_start));
        return emit_at_rule(
            name,
            prelude,
            {},
            false,
            context,
            owner_rule_index,
            start);
    }

    bool recover_bad_declaration(bool* closed_rule) {
        if (closed_rule == nullptr) {
            return fail(
                CssParserV1ErrorKind::InvalidConfiguration,
                cursor_,
                "CSS recovery closed-rule output is null");
        }
        *closed_rule = false;
        std::array<char, kMaximumConfiguredNestingDepth> stack{};
        std::uint32_t depth = 0U;

        while (cursor_ < input_.size()) {
            const char current = input_[cursor_];
            if (current == '"' || current == '\'') {
                if (!consume_string(current)) {
                    return false;
                }
                continue;
            }
            if (cursor_ + 1U < input_.size() &&
                current == '/' &&
                input_[cursor_ + 1U] == '*') {
                if (!skip_comment()) {
                    return false;
                }
                continue;
            }
            if (current == '\\') {
                ++cursor_;
                if (cursor_ < input_.size()) {
                    ++cursor_;
                }
                continue;
            }
            if (current == '(' || current == '[' ||
                current == '{') {
                const char closer =
                    current == '(' ? ')' :
                    (current == '[' ? ']' : '}');
                if (!push_nesting(
                        closer,
                        cursor_,
                        &stack,
                        &depth)) {
                    return false;
                }
                ++cursor_;
                continue;
            }
            if (current == ')' || current == ']') {
                if (depth != 0U &&
                    stack[depth - 1U] == current) {
                    --depth;
                }
                ++cursor_;
                continue;
            }
            if (current == '}') {
                if (depth != 0U) {
                    if (stack[depth - 1U] == '}') {
                        --depth;
                    }
                    ++cursor_;
                    continue;
                }
                ++cursor_;
                *closed_rule = true;
                return true;
            }
            if (current == ';' && depth == 0U) {
                ++cursor_;
                return true;
            }
            ++cursor_;
        }
        return fail(
            CssParserV1ErrorKind::UnbalancedBlock,
            cursor_,
            "CSS declaration recovery reached EOF before rule close");
    }

    bool parse_selector(std::string_view* selector) {
        const std::size_t start = cursor_;
        std::array<char, kMaximumConfiguredNestingDepth> stack{};
        std::uint32_t depth = 0U;
        while (cursor_ < input_.size()) {
            const char value = input_[cursor_];
            if (value == '"' || value == '\'') {
                if (!consume_string(value)) {
                    return false;
                }
                continue;
            }
            if (cursor_ + 1U < input_.size() && value == '/' && input_[cursor_ + 1U] == '*') {
                if (!skip_comment()) {
                    return false;
                }
                continue;
            }
            if (value == '\\') {
                ++cursor_;
                if (cursor_ == input_.size()) {
                    return fail(CssParserV1ErrorKind::InvalidSelector, start, "dangling CSS selector escape");
                }
                ++cursor_;
                continue;
            }
            if (value == '(') {
                if (!push_nesting(')', cursor_, &stack, &depth)) {
                    return false;
                }
                ++cursor_;
                continue;
            }
            if (value == '[') {
                if (!push_nesting(']', cursor_, &stack, &depth)) {
                    return false;
                }
                ++cursor_;
                continue;
            }
            if (value == ')' || value == ']') {
                if (depth == 0U || stack[depth - 1U] != value) {
                    return fail(CssParserV1ErrorKind::InvalidSelector, cursor_, "mismatched CSS selector delimiter");
                }
                --depth;
                ++cursor_;
                continue;
            }
            if (value == '{' && depth == 0U) {
                *selector = trim_ascii(input_.substr(start, cursor_ - start));
                if (selector->empty()) {
                    return fail(CssParserV1ErrorKind::InvalidSelector, start, "empty CSS qualified-rule selector");
                }
                ++cursor_;
                return true;
            }
            if (value == '}' && depth == 0U) {
                return fail(CssParserV1ErrorKind::InvalidSelector, cursor_, "CSS selector closed before declaration block");
            }
            ++cursor_;
        }
        return fail(CssParserV1ErrorKind::UnbalancedBlock, start, "CSS qualified rule has no declaration block");
    }

    bool scan_value(std::size_t value_start, std::string_view* value, bool* closed_rule) {
        std::array<char, kMaximumConfiguredNestingDepth> stack{};
        std::uint32_t depth = 0U;
        while (cursor_ < input_.size()) {
            const char current = input_[cursor_];
            if (current == '"' || current == '\'') {
                if (!consume_string(current)) {
                    return false;
                }
                continue;
            }
            if (cursor_ + 1U < input_.size() && current == '/' && input_[cursor_ + 1U] == '*') {
                if (!skip_comment()) {
                    return false;
                }
                continue;
            }
            if (current == '\\') {
                ++cursor_;
                if (cursor_ == input_.size()) {
                    return fail(CssParserV1ErrorKind::InvalidDeclaration, value_start, "dangling CSS declaration escape");
                }
                ++cursor_;
                continue;
            }
            if (current == '(' || current == '[' || current == '{') {
                const char closer = current == '(' ? ')' : (current == '[' ? ']' : '}');
                if (!push_nesting(closer, cursor_, &stack, &depth)) {
                    return false;
                }
                ++cursor_;
                continue;
            }
            if (current == ')' || current == ']' || current == '}') {
                if (depth != 0U) {
                    if (stack[depth - 1U] != current) {
                        return fail(CssParserV1ErrorKind::InvalidDeclaration, cursor_, "mismatched CSS declaration delimiter");
                    }
                    --depth;
                    ++cursor_;
                    continue;
                }
                if (current == '}') {
                    *value = trim_ascii(input_.substr(value_start, cursor_ - value_start));
                    ++cursor_;
                    *closed_rule = true;
                    return true;
                }
                return fail(CssParserV1ErrorKind::InvalidDeclaration, cursor_, "unexpected CSS declaration closing delimiter");
            }
            if (current == ';' && depth == 0U) {
                *value = trim_ascii(input_.substr(value_start, cursor_ - value_start));
                ++cursor_;
                *closed_rule = false;
                return true;
            }
            ++cursor_;
        }
        return fail(CssParserV1ErrorKind::UnbalancedBlock, value_start, "CSS declaration block is unterminated");
    }

    bool parse_declaration(bool* closed_rule) {
        const std::size_t property_start = cursor_;
        while (cursor_ < input_.size() && input_[cursor_] != ':') {
            if (input_[cursor_] == ';' || input_[cursor_] == '}') {
                return fail(CssParserV1ErrorKind::InvalidDeclaration, property_start, "CSS declaration is missing a colon");
            }
            if (cursor_ + 1U < input_.size() && input_[cursor_] == '/' && input_[cursor_ + 1U] == '*') {
                return fail(CssParserV1ErrorKind::InvalidDeclaration, cursor_, "comments inside CSS property names are outside the parser foundation scope");
            }
            ++cursor_;
        }
        if (cursor_ == input_.size()) {
            return fail(CssParserV1ErrorKind::UnbalancedBlock, property_start, "CSS declaration block is unterminated");
        }
        const std::string_view property = trim_ascii(input_.substr(property_start, cursor_ - property_start));
        if (!valid_property_name(property)) {
            return fail(CssParserV1ErrorKind::InvalidDeclaration, property_start, "invalid CSS property name in foundation grammar");
        }
        ++cursor_;
        const std::size_t value_start = cursor_;
        std::string_view raw_value;
        if (!scan_value(value_start, &raw_value, closed_rule)) {
            return false;
        }
        const ImportantSplit important = split_important(raw_value);
        if (output_->declarations.size() >= config_.maximum_declarations) {
            return fail(CssParserV1ErrorKind::DeclarationLimitExceeded, property_start, "CSS declaration count exceeds configured limit");
        }
        CssDeclarationV1 declaration;
        const bool custom = property.size() > 2U && property[0] == '-' && property[1] == '-';
        if (!append_slice(property, !custom, &declaration.property) ||
            !append_slice(important.value, false, &declaration.value)) {
            return false;
        }
        declaration.important = important.important;
        output_->declarations.push_back(declaration);
        ++stats_->declarations;
        if (declaration.important) {
            ++stats_->important_declarations;
        }
        return true;
    }

    bool parse_style_rule() {
        if (output_->rules.size() >= config_.maximum_rules) {
            return fail(CssParserV1ErrorKind::RuleLimitExceeded, cursor_, "CSS rule count exceeds configured limit");
        }
        const std::size_t rule_start = cursor_;
        std::string_view selector;
        if (!parse_selector(&selector)) {
            return false;
        }
        CssStyleRuleV1 rule;
        if (!append_slice(selector, false, &rule.selector)) {
            return false;
        }
        if (output_->declarations.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            return fail(CssParserV1ErrorKind::DeclarationLimitExceeded, rule_start, "CSS declaration index exceeds 32-bit representation");
        }
        rule.declaration_offset = static_cast<std::uint32_t>(output_->declarations.size());
        bool closed_rule = false;
        while (!closed_rule) {
            if (!skip_trivia()) {
                return false;
            }
            if (cursor_ == input_.size()) {
                return fail(CssParserV1ErrorKind::UnbalancedBlock, rule_start, "CSS declaration block is unterminated");
            }
            if (input_[cursor_] == '}') {
                ++cursor_;
                closed_rule = true;
                break;
            }
            if (input_[cursor_] == ';') {
                ++cursor_;
                continue;
            }
            if (input_[cursor_] == '@') {
                if (output_->rules.size() >
                    static_cast<std::size_t>(
                        std::numeric_limits<std::uint32_t>::max())) {
                    return fail(
                        CssParserV1ErrorKind::RuleLimitExceeded,
                        rule_start,
                        "CSS at-rule owner rule index exceeds 32-bit representation");
                }
                if (!parse_at_rule(
                        CssAtRuleContextV1::DeclarationList,
                        static_cast<std::uint32_t>(
                            output_->rules.size()))) {
                    return false;
                }
                continue;
            }
            if (!parse_declaration(&closed_rule)) {
                if (error_->kind !=
                    CssParserV1ErrorKind::InvalidDeclaration) {
                    return false;
                }
                ++stats_->recovered_invalid_declarations;
                clear_error_after_recovery();
                if (!recover_bad_declaration(&closed_rule)) {
                    return false;
                }
            }
        }
        const std::size_t declaration_count = output_->declarations.size() - static_cast<std::size_t>(rule.declaration_offset);
        if (declaration_count > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            return fail(CssParserV1ErrorKind::DeclarationLimitExceeded, rule_start, "CSS rule declaration count exceeds 32-bit representation");
        }
        rule.declaration_count = static_cast<std::uint32_t>(declaration_count);
        output_->rules.push_back(rule);
        ++stats_->rules;
        return true;
    }

    std::string_view input_;
    CssParserV1Config config_{};
    CssStylesheetV1* output_{nullptr};
    CssParserV1Stats* stats_{nullptr};
    CssParserV1Error* error_{nullptr};
    std::size_t cursor_{0U};
};

} // namespace

bool CssParserV1Config::valid() const noexcept {
    return maximum_input_bytes > 0U && maximum_input_bytes <= kMaximumConfiguredInputBytes &&
        maximum_rules > 0U && maximum_rules <= kMaximumConfiguredRules &&
        maximum_declarations > 0U && maximum_declarations <= kMaximumConfiguredDeclarations &&
        maximum_at_rules > 0U && maximum_at_rules <= kMaximumConfiguredAtRules &&
        maximum_nesting_depth > 0U && maximum_nesting_depth <= kMaximumConfiguredNestingDepth &&
        maximum_output_text_bytes > 0U &&
        maximum_output_text_bytes <= kMaximumConfiguredOutputTextBytes &&
        maximum_output_text_bytes <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
}

CssStylesheetV1::CssStylesheetV1(std::pmr::memory_resource* memory)
    : text(memory), rules(memory), declarations(memory), at_rules(memory) {}

std::pmr::memory_resource* CssStylesheetV1::resource() const noexcept {
    return text.get_allocator().resource();
}

std::string_view CssStylesheetV1::resolve(CssTextSliceV1 slice) const noexcept {
    const std::size_t offset = static_cast<std::size_t>(slice.offset);
    const std::size_t length = static_cast<std::size_t>(slice.length);
    if (offset > text.size() || length > text.size() - offset) {
        return {};
    }
    return std::string_view(text.data() + offset, length);
}

void CssStylesheetV1::release() noexcept {
    std::pmr::string empty_text(resource());
    std::pmr::vector<CssStyleRuleV1> empty_rules(resource());
    std::pmr::vector<CssDeclarationV1> empty_declarations(resource());
    std::pmr::vector<CssAtRuleV1> empty_at_rules(resource());
    text.swap(empty_text);
    rules.swap(empty_rules);
    declarations.swap(empty_declarations);
    at_rules.swap(empty_at_rules);
}

const char* css_parser_v1_error_kind_name(CssParserV1ErrorKind kind) noexcept {
    switch (kind) {
    case CssParserV1ErrorKind::None: return "none";
    case CssParserV1ErrorKind::InvalidConfiguration: return "invalid-configuration";
    case CssParserV1ErrorKind::InputTooLarge: return "input-too-large";
    case CssParserV1ErrorKind::UnsupportedSyntax: return "unsupported-syntax";
    case CssParserV1ErrorKind::InvalidSelector: return "invalid-selector";
    case CssParserV1ErrorKind::InvalidDeclaration: return "invalid-declaration";
    case CssParserV1ErrorKind::InvalidAtRule: return "invalid-at-rule";
    case CssParserV1ErrorKind::UnterminatedComment: return "unterminated-comment";
    case CssParserV1ErrorKind::UnterminatedString: return "unterminated-string";
    case CssParserV1ErrorKind::UnbalancedBlock: return "unbalanced-block";
    case CssParserV1ErrorKind::NestingLimitExceeded: return "nesting-limit-exceeded";
    case CssParserV1ErrorKind::RuleLimitExceeded: return "rule-limit-exceeded";
    case CssParserV1ErrorKind::DeclarationLimitExceeded: return "declaration-limit-exceeded";
    case CssParserV1ErrorKind::AtRuleLimitExceeded: return "at-rule-limit-exceeded";
    case CssParserV1ErrorKind::OutputBudgetExceeded: return "output-budget-exceeded";
    case CssParserV1ErrorKind::AllocationFailure: return "allocation-failure";
    }
    return "unknown";
}

bool parse_css_stylesheet_v1(
    std::string_view input,
    CssParserV1Config config,
    CssStylesheetV1* output,
    CssParserV1Stats* stats,
    CssParserV1Error* error) noexcept {
    if (stats != nullptr) {
        *stats = CssParserV1Stats{};
    }
    if (error != nullptr) {
        error->kind = CssParserV1ErrorKind::None;
        error->byte_offset = 0U;
        error->message.clear();
    }
    if (output == nullptr || stats == nullptr || error == nullptr || output->resource() == nullptr) {
        return set_error(error, CssParserV1ErrorKind::InvalidConfiguration, 0U, "CSS parser output, stats, error and memory resource are required");
    }
    if (!config.valid()) {
        return set_error(error, CssParserV1ErrorKind::InvalidConfiguration, 0U, "CSS parser configuration is invalid");
    }
    if (input.size() > config.maximum_input_bytes) {
        return set_error(error, CssParserV1ErrorKind::InputTooLarge, config.maximum_input_bytes, "CSS input exceeds configured byte limit");
    }

    CssStylesheetV1 candidate(output->resource());
    CssParserV1Stats candidate_stats;
    candidate_stats.input_bytes = static_cast<std::uint64_t>(input.size());
    try {
        std::pmr::string preprocessed(output->resource());
        preprocess_css_input(input, &preprocessed, &candidate_stats);
        const std::string_view parser_input(
            preprocessed.data(), preprocessed.size());
        Parser parser(parser_input, config, &candidate, &candidate_stats, error);
        if (!parser.run()) {
            *stats = candidate_stats;
            return false;
        }
        output->release();
        output->text.swap(candidate.text);
        output->rules.swap(candidate.rules);
        output->declarations.swap(candidate.declarations);
        output->at_rules.swap(candidate.at_rules);
        *stats = candidate_stats;
        return true;
    } catch (const std::bad_alloc&) {
        *stats = candidate_stats;
        return set_error(error, CssParserV1ErrorKind::AllocationFailure, 0U, "CSS parser allocation rejected by bounded memory resource");
    } catch (...) {
        *stats = candidate_stats;
        return set_error(error, CssParserV1ErrorKind::AllocationFailure, 0U, "CSS parser allocation or container operation failed");
    }
}

bool parse_css_declaration_list_v1(
    std::string_view input,
    CssParserV1Config config,
    CssStylesheetV1* output,
    CssParserV1Stats* stats,
    CssParserV1Error* error) noexcept {
    if (stats != nullptr) {
        *stats = CssParserV1Stats{};
    }
    if (error != nullptr) {
        error->kind = CssParserV1ErrorKind::None;
        error->byte_offset = 0U;
        error->message.clear();
    }
    if (output == nullptr || stats == nullptr || error == nullptr ||
        output->resource() == nullptr) {
        return set_error(
            error,
            CssParserV1ErrorKind::InvalidConfiguration,
            0U,
            "CSS declaration-list parser output, stats, error and memory resource are required");
    }
    if (!config.valid()) {
        return set_error(
            error,
            CssParserV1ErrorKind::InvalidConfiguration,
            0U,
            "CSS declaration-list parser configuration is invalid");
    }
    if (input.size() > config.maximum_input_bytes) {
        return set_error(
            error,
            CssParserV1ErrorKind::InputTooLarge,
            config.maximum_input_bytes,
            "CSS declaration-list input exceeds configured byte limit");
    }

    CssStylesheetV1 candidate(output->resource());
    CssParserV1Stats candidate_stats;
    candidate_stats.input_bytes =
        static_cast<std::uint64_t>(input.size());
    try {
        std::pmr::string preprocessed(output->resource());
        preprocess_css_input(
            input,
            &preprocessed,
            &candidate_stats);

        // The existing declaration parser is intentionally block-aware. Add
        // one internal close sentinel after preprocessing so EOF termination
        // reuses the exact same value scanning and recovery path. The sentinel
        // is not counted in input/preprocessed statistics and any user-provided
        // closing brace is detected because it closes before this final byte.
        preprocessed.push_back('}');

        const std::string_view parser_input(
            preprocessed.data(),
            preprocessed.size());
        Parser parser(
            parser_input,
            config,
            &candidate,
            &candidate_stats,
            error);
        if (!parser.run_declaration_list()) {
            *stats = candidate_stats;
            return false;
        }

        output->release();
        output->text.swap(candidate.text);
        output->rules.swap(candidate.rules);
        output->declarations.swap(candidate.declarations);
        output->at_rules.swap(candidate.at_rules);
        *stats = candidate_stats;
        return true;
    } catch (const std::bad_alloc&) {
        *stats = candidate_stats;
        return set_error(
            error,
            CssParserV1ErrorKind::AllocationFailure,
            0U,
            "CSS declaration-list parser allocation rejected by bounded memory resource");
    } catch (...) {
        *stats = candidate_stats;
        return set_error(
            error,
            CssParserV1ErrorKind::AllocationFailure,
            0U,
            "CSS declaration-list parser allocation or container operation failed");
    }
}


} // namespace zevryon::style
