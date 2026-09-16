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
                return fail(CssParserV1ErrorKind::UnsupportedSyntax, cursor_, "CSS at-rules are outside the parser foundation scope");
            }
            if (!parse_style_rule()) {
                return false;
            }
        }
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
            if (!parse_declaration(&closed_rule)) {
                return false;
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
        maximum_nesting_depth > 0U && maximum_nesting_depth <= kMaximumConfiguredNestingDepth &&
        maximum_output_text_bytes > 0U &&
        maximum_output_text_bytes <= kMaximumConfiguredOutputTextBytes &&
        maximum_output_text_bytes <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
}

CssStylesheetV1::CssStylesheetV1(std::pmr::memory_resource* memory)
    : text(memory), rules(memory), declarations(memory) {}

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
    text.swap(empty_text);
    rules.swap(empty_rules);
    declarations.swap(empty_declarations);
}

const char* css_parser_v1_error_kind_name(CssParserV1ErrorKind kind) noexcept {
    switch (kind) {
    case CssParserV1ErrorKind::None: return "none";
    case CssParserV1ErrorKind::InvalidConfiguration: return "invalid-configuration";
    case CssParserV1ErrorKind::InputTooLarge: return "input-too-large";
    case CssParserV1ErrorKind::UnsupportedSyntax: return "unsupported-syntax";
    case CssParserV1ErrorKind::InvalidSelector: return "invalid-selector";
    case CssParserV1ErrorKind::InvalidDeclaration: return "invalid-declaration";
    case CssParserV1ErrorKind::UnterminatedComment: return "unterminated-comment";
    case CssParserV1ErrorKind::UnterminatedString: return "unterminated-string";
    case CssParserV1ErrorKind::UnbalancedBlock: return "unbalanced-block";
    case CssParserV1ErrorKind::NestingLimitExceeded: return "nesting-limit-exceeded";
    case CssParserV1ErrorKind::RuleLimitExceeded: return "rule-limit-exceeded";
    case CssParserV1ErrorKind::DeclarationLimitExceeded: return "declaration-limit-exceeded";
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
        Parser parser(input, config, &candidate, &candidate_stats, error);
        if (!parser.run()) {
            *stats = candidate_stats;
            return false;
        }
        output->release();
        output->text.swap(candidate.text);
        output->rules.swap(candidate.rules);
        output->declarations.swap(candidate.declarations);
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

} // namespace zevryon::style
