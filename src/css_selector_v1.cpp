#include "css_selector_v1.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace zevryon::style {
namespace {

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

std::string_view trim_ascii(std::string_view value) noexcept {
    while (!value.empty() && ascii_space(value.front())) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && ascii_space(value.back())) {
        value.remove_suffix(1U);
    }
    return value;
}

bool identifier_start(char value) noexcept {
    const unsigned char byte = static_cast<unsigned char>(value);
    return ascii_alpha(value) || value == '_' || value == '-' || byte >= 0x80U;
}

bool identifier_continue(char value) noexcept {
    return identifier_start(value) || ascii_digit(value);
}

bool set_error(
    CssSelectorCompileErrorV1* error,
    CssSelectorCompileErrorKindV1 kind,
    std::size_t offset,
    const char* message) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->byte_offset = offset;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

class Compiler final {
public:
    Compiler(
        std::string_view input,
        CssSelectorCompileConfigV1 config,
        CssCompoundSelectorV1* output,
        CssSelectorCompileStatsV1* stats,
        CssSelectorCompileErrorV1* error)
        : input_(input),
          config_(config),
          output_(output),
          stats_(stats),
          error_(error) {}

    bool run() {
        input_ = trim_ascii(input_);
        if (input_.empty()) {
            return fail(
                CssSelectorCompileErrorKindV1::EmptySelector,
                0U,
                "CSS compound selector is empty");
        }

        if (input_.front() == '*') {
            if (!append_simple(CssSelectorSimpleKindV1::Universal, {}, {})) {
                return false;
            }
            ++stats_->universal_selectors;
            ++cursor_;
        } else if (identifier_start(input_.front())) {
            const std::size_t start = cursor_;
            std::string_view name;
            if (!consume_identifier(&name)) {
                return false;
            }
            if (!append_simple(
                    CssSelectorSimpleKindV1::Type,
                    name,
                    {},
                    true)) {
                return false;
            }
            ++stats_->type_selectors;
            if (!increment_specificity(&output_->specificity.types, start)) {
                return false;
            }
        }

        while (cursor_ < input_.size()) {
            const char current = input_[cursor_];
            if (current == '#') {
                const std::size_t start = cursor_++;
                std::string_view name;
                if (!consume_identifier(&name)) {
                    return false;
                }
                if (!append_simple(CssSelectorSimpleKindV1::Id, name, {})) {
                    return false;
                }
                ++stats_->id_selectors;
                if (!increment_specificity(&output_->specificity.ids, start)) {
                    return false;
                }
                continue;
            }
            if (current == '.') {
                const std::size_t start = cursor_++;
                std::string_view name;
                if (!consume_identifier(&name)) {
                    return false;
                }
                if (!append_simple(CssSelectorSimpleKindV1::Class, name, {})) {
                    return false;
                }
                ++stats_->class_selectors;
                if (!increment_specificity(&output_->specificity.classes, start)) {
                    return false;
                }
                continue;
            }
            if (current == '[') {
                if (!parse_attribute()) {
                    return false;
                }
                continue;
            }
            if (ascii_space(current)) {
                return fail(
                    CssSelectorCompileErrorKindV1::UnsupportedSyntax,
                    cursor_,
                    "CSS combinators are outside the compound-selector foundation");
            }
            if (current == ':' || current == ',' || current == '>' ||
                current == '+' || current == '~') {
                return fail(
                    CssSelectorCompileErrorKindV1::UnsupportedSyntax,
                    cursor_,
                    "CSS selector syntax is outside the compound-selector foundation");
            }
            if (current == '\\') {
                return fail(
                    CssSelectorCompileErrorKindV1::UnsupportedSyntax,
                    cursor_,
                    "CSS identifier escapes are outside the selector foundation");
            }
            return fail(
                CssSelectorCompileErrorKindV1::InvalidIdentifier,
                cursor_,
                "invalid byte in CSS compound selector");
        }

        if (output_->simple.empty()) {
            return fail(
                CssSelectorCompileErrorKindV1::EmptySelector,
                0U,
                "CSS compound selector contains no simple selector");
        }
        stats_->simple_selectors =
            static_cast<std::uint64_t>(output_->simple.size());
        return true;
    }

private:
    bool fail(
        CssSelectorCompileErrorKindV1 kind,
        std::size_t offset,
        const char* message) noexcept {
        return set_error(error_, kind, offset, message);
    }

    bool increment_specificity(
        std::uint32_t* value,
        std::size_t offset) noexcept {
        if (*value == std::numeric_limits<std::uint32_t>::max()) {
            return fail(
                CssSelectorCompileErrorKindV1::SpecificityOverflow,
                offset,
                "CSS selector specificity overflows 32-bit component");
        }
        ++(*value);
        return true;
    }

    bool consume_identifier(std::string_view* value) {
        const std::size_t start = cursor_;
        if (cursor_ >= input_.size() || !identifier_start(input_[cursor_])) {
            return fail(
                CssSelectorCompileErrorKindV1::InvalidIdentifier,
                start,
                "CSS selector identifier is missing");
        }
        if (input_[cursor_] == '-') {
            if (cursor_ + 1U >= input_.size() ||
                !identifier_start(input_[cursor_ + 1U])) {
                return fail(
                    CssSelectorCompileErrorKindV1::InvalidIdentifier,
                    start,
                    "CSS selector identifier has invalid hyphen start");
            }
        }
        ++cursor_;
        while (cursor_ < input_.size() &&
               identifier_continue(input_[cursor_])) {
            ++cursor_;
        }
        *value = input_.substr(start, cursor_ - start);
        return true;
    }

    void skip_space() noexcept {
        while (cursor_ < input_.size() && ascii_space(input_[cursor_])) {
            ++cursor_;
        }
    }

    bool append_slice(
        std::string_view value,
        bool lowercase,
        CssSelectorTextSliceV1* slice) {
        if (slice == nullptr) {
            return fail(
                CssSelectorCompileErrorKindV1::InvalidConfiguration,
                cursor_,
                "CSS selector output slice is null");
        }
        if (output_->text.size() > config_.maximum_output_text_bytes ||
            value.size() >
                config_.maximum_output_text_bytes - output_->text.size()) {
            return fail(
                CssSelectorCompileErrorKindV1::OutputBudgetExceeded,
                cursor_,
                "CSS selector output text budget exceeded");
        }
        if (output_->text.size() >
                static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max()) ||
            value.size() >
                static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())) {
            return fail(
                CssSelectorCompileErrorKindV1::OutputBudgetExceeded,
                cursor_,
                "CSS selector text slice exceeds 32-bit representation");
        }

        slice->offset =
            static_cast<std::uint32_t>(output_->text.size());
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

    bool append_simple(
        CssSelectorSimpleKindV1 kind,
        std::string_view name,
        std::string_view value,
        bool lowercase_name = false) {
        if (output_->simple.size() >= config_.maximum_simple_selectors) {
            return fail(
                CssSelectorCompileErrorKindV1::SimpleSelectorLimitExceeded,
                cursor_,
                "CSS selector exceeds configured simple-selector limit");
        }
        CssSelectorSimpleV1 simple;
        simple.kind = kind;
        if (!name.empty() &&
            !append_slice(name, lowercase_name, &simple.name)) {
            return false;
        }
        if (!value.empty() &&
            !append_slice(value, false, &simple.value)) {
            return false;
        }
        output_->simple.push_back(simple);
        return true;
    }

    bool parse_attribute_value(std::string_view* value) {
        if (cursor_ >= input_.size()) {
            return fail(
                CssSelectorCompileErrorKindV1::InvalidAttribute,
                cursor_,
                "CSS attribute selector value is missing");
        }

        const char first = input_[cursor_];
        if (first == '"' || first == '\'') {
            const char quote = first;
            const std::size_t start = ++cursor_;
            while (cursor_ < input_.size() && input_[cursor_] != quote) {
                if (input_[cursor_] == '\\') {
                    return fail(
                        CssSelectorCompileErrorKindV1::UnsupportedSyntax,
                        cursor_,
                        "CSS attribute-value escapes are outside the selector foundation");
                }
                if (input_[cursor_] == '\n' ||
                    input_[cursor_] == '\r' ||
                    input_[cursor_] == '\f') {
                    return fail(
                        CssSelectorCompileErrorKindV1::InvalidAttribute,
                        cursor_,
                        "CSS quoted attribute value contains a newline");
                }
                ++cursor_;
            }
            if (cursor_ == input_.size()) {
                return fail(
                    CssSelectorCompileErrorKindV1::InvalidAttribute,
                    start,
                    "CSS quoted attribute selector value is unterminated");
            }
            *value = input_.substr(start, cursor_ - start);
            ++cursor_;
            return true;
        }

        return consume_identifier(value);
    }

    bool parse_attribute() {
        const std::size_t start = cursor_++;
        skip_space();

        std::string_view name;
        if (!consume_identifier(&name)) {
            return false;
        }
        skip_space();

        if (cursor_ >= input_.size()) {
            return fail(
                CssSelectorCompileErrorKindV1::InvalidAttribute,
                start,
                "CSS attribute selector is unterminated");
        }
        if (input_[cursor_] == ']') {
            ++cursor_;
            if (!append_simple(
                    CssSelectorSimpleKindV1::AttributeExists,
                    name,
                    {},
                    true)) {
                return false;
            }
            ++stats_->attribute_selectors;
            return increment_specificity(
                &output_->specificity.classes,
                start);
        }

        if (input_[cursor_] != '=') {
            if (input_[cursor_] == '~' || input_[cursor_] == '|' ||
                input_[cursor_] == '^' || input_[cursor_] == '$' ||
                input_[cursor_] == '*') {
                return fail(
                    CssSelectorCompileErrorKindV1::UnsupportedSyntax,
                    cursor_,
                    "CSS attribute operators other than '=' are outside the selector foundation");
            }
            return fail(
                CssSelectorCompileErrorKindV1::InvalidAttribute,
                cursor_,
                "CSS attribute selector requires ']' or '='");
        }
        ++cursor_;
        skip_space();

        std::string_view value;
        if (!parse_attribute_value(&value)) {
            return false;
        }
        skip_space();
        if (cursor_ >= input_.size() || input_[cursor_] != ']') {
            return fail(
                CssSelectorCompileErrorKindV1::InvalidAttribute,
                cursor_,
                "CSS attribute selector is missing closing bracket");
        }
        ++cursor_;

        if (!append_simple(
                CssSelectorSimpleKindV1::AttributeEquals,
                name,
                value,
                true)) {
            return false;
        }
        ++stats_->attribute_selectors;
        return increment_specificity(
            &output_->specificity.classes,
            start);
    }

    std::string_view input_;
    CssSelectorCompileConfigV1 config_{};
    CssCompoundSelectorV1* output_{nullptr};
    CssSelectorCompileStatsV1* stats_{nullptr};
    CssSelectorCompileErrorV1* error_{nullptr};
    std::size_t cursor_{0U};
};

bool class_token_matches(
    std::string_view classes,
    std::string_view expected) noexcept {
    std::size_t cursor = 0U;
    while (cursor < classes.size()) {
        while (cursor < classes.size() && ascii_space(classes[cursor])) {
            ++cursor;
        }
        const std::size_t start = cursor;
        while (cursor < classes.size() && !ascii_space(classes[cursor])) {
            ++cursor;
        }
        if (cursor > start &&
            classes.substr(start, cursor - start) == expected) {
            return true;
        }
    }
    return false;
}

const CssSelectorAttributeV1* find_attribute(
    const CssSelectorNodeV1& node,
    std::string_view name) noexcept {
    for (const CssSelectorAttributeV1& attribute : node.attributes) {
        if (ascii_iequals(attribute.name, name)) {
            return &attribute;
        }
    }
    return nullptr;
}

} // namespace

int compare_css_specificity_v1(
    CssSpecificityV1 left,
    CssSpecificityV1 right) noexcept {
    if (left.ids != right.ids) {
        return left.ids < right.ids ? -1 : 1;
    }
    if (left.classes != right.classes) {
        return left.classes < right.classes ? -1 : 1;
    }
    if (left.types != right.types) {
        return left.types < right.types ? -1 : 1;
    }
    return 0;
}

CssCompoundSelectorV1::CssCompoundSelectorV1(
    std::pmr::memory_resource* memory)
    : text(memory), simple(memory) {}

std::pmr::memory_resource*
CssCompoundSelectorV1::resource() const noexcept {
    return text.get_allocator().resource();
}

std::string_view CssCompoundSelectorV1::resolve(
    CssSelectorTextSliceV1 slice) const noexcept {
    const std::size_t offset = static_cast<std::size_t>(slice.offset);
    const std::size_t length = static_cast<std::size_t>(slice.length);
    if (offset > text.size() || length > text.size() - offset) {
        return {};
    }
    return std::string_view(text.data() + offset, length);
}

void CssCompoundSelectorV1::release() noexcept {
    std::pmr::string empty_text(resource());
    std::pmr::vector<CssSelectorSimpleV1> empty_simple(resource());
    text.swap(empty_text);
    simple.swap(empty_simple);
    specificity = CssSpecificityV1{};
}

bool CssSelectorCompileConfigV1::valid() const noexcept {
    return maximum_input_bytes > 0U &&
        maximum_input_bytes <= kMaximumInputBytesLimit &&
        maximum_simple_selectors > 0U &&
        maximum_simple_selectors <= kMaximumSimpleSelectorsLimit &&
        maximum_output_text_bytes > 0U &&
        maximum_output_text_bytes <= kMaximumOutputTextBytesLimit &&
        maximum_output_text_bytes <=
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max());
}

bool CssSelectorMatchConfigV1::valid() const noexcept {
    return maximum_attributes > 0U &&
        maximum_attributes <= kMaximumAttributesLimit &&
        maximum_semantic_bytes > 0U &&
        maximum_semantic_bytes <= kMaximumSemanticBytesLimit &&
        maximum_work_units > 0U &&
        maximum_work_units <= kMaximumWorkUnitsLimit;
}

const char* css_selector_compile_error_kind_name_v1(
    CssSelectorCompileErrorKindV1 kind) noexcept {
    switch (kind) {
    case CssSelectorCompileErrorKindV1::None:
        return "none";
    case CssSelectorCompileErrorKindV1::InvalidConfiguration:
        return "invalid-configuration";
    case CssSelectorCompileErrorKindV1::EmptySelector:
        return "empty-selector";
    case CssSelectorCompileErrorKindV1::InputTooLarge:
        return "input-too-large";
    case CssSelectorCompileErrorKindV1::UnsupportedSyntax:
        return "unsupported-syntax";
    case CssSelectorCompileErrorKindV1::InvalidIdentifier:
        return "invalid-identifier";
    case CssSelectorCompileErrorKindV1::InvalidAttribute:
        return "invalid-attribute";
    case CssSelectorCompileErrorKindV1::SimpleSelectorLimitExceeded:
        return "simple-selector-limit-exceeded";
    case CssSelectorCompileErrorKindV1::OutputBudgetExceeded:
        return "output-budget-exceeded";
    case CssSelectorCompileErrorKindV1::SpecificityOverflow:
        return "specificity-overflow";
    case CssSelectorCompileErrorKindV1::AllocationFailure:
        return "allocation-failure";
    }
    return "unknown";
}

bool compile_css_compound_selector_v1(
    std::string_view input,
    CssSelectorCompileConfigV1 config,
    CssCompoundSelectorV1* output,
    CssSelectorCompileStatsV1* stats,
    CssSelectorCompileErrorV1* error) noexcept {
    if (stats != nullptr) {
        *stats = CssSelectorCompileStatsV1{};
    }
    if (error != nullptr) {
        error->kind = CssSelectorCompileErrorKindV1::None;
        error->byte_offset = 0U;
        error->message.clear();
    }
    if (output == nullptr || stats == nullptr || error == nullptr ||
        output->resource() == nullptr) {
        return set_error(
            error,
            CssSelectorCompileErrorKindV1::InvalidConfiguration,
            0U,
            "CSS selector output, stats, error and memory resource are required");
    }
    if (!config.valid()) {
        return set_error(
            error,
            CssSelectorCompileErrorKindV1::InvalidConfiguration,
            0U,
            "CSS selector compiler configuration is invalid");
    }
    if (input.size() > config.maximum_input_bytes) {
        return set_error(
            error,
            CssSelectorCompileErrorKindV1::InputTooLarge,
            config.maximum_input_bytes,
            "CSS selector input exceeds configured byte limit");
    }

    CssCompoundSelectorV1 candidate(output->resource());
    CssSelectorCompileStatsV1 candidate_stats;
    candidate_stats.input_bytes =
        static_cast<std::uint64_t>(input.size());
    try {
        Compiler compiler(
            input,
            config,
            &candidate,
            &candidate_stats,
            error);
        if (!compiler.run()) {
            *stats = candidate_stats;
            return false;
        }
        output->release();
        output->text.swap(candidate.text);
        output->simple.swap(candidate.simple);
        output->specificity = candidate.specificity;
        *stats = candidate_stats;
        return true;
    } catch (const std::bad_alloc&) {
        *stats = candidate_stats;
        return set_error(
            error,
            CssSelectorCompileErrorKindV1::AllocationFailure,
            0U,
            "CSS selector allocation rejected by bounded memory resource");
    } catch (...) {
        *stats = candidate_stats;
        return set_error(
            error,
            CssSelectorCompileErrorKindV1::AllocationFailure,
            0U,
            "CSS selector allocation or container operation failed");
    }
}

bool match_css_compound_selector_v1(
    const CssCompoundSelectorV1& selector,
    const CssSelectorNodeV1& node,
    CssSelectorMatchConfigV1 config,
    bool* matched) noexcept {
    if (matched == nullptr) {
        return false;
    }
    *matched = false;
    if (!config.valid() ||
        selector.simple.empty() ||
        selector.simple.size() >
            CssSelectorCompileConfigV1::kMaximumSimpleSelectorsLimit ||
        node.attributes.size() > config.maximum_attributes) {
        return false;
    }

    std::size_t semantic_bytes = node.tag.size();
    if (semantic_bytes > config.maximum_semantic_bytes) {
        return false;
    }
    for (const CssSelectorAttributeV1& attribute : node.attributes) {
        if (attribute.name.size() >
                config.maximum_semantic_bytes - semantic_bytes) {
            return false;
        }
        semantic_bytes += attribute.name.size();
        if (attribute.value.size() >
                config.maximum_semantic_bytes - semantic_bytes) {
            return false;
        }
        semantic_bytes += attribute.value.size();
    }

    std::uint64_t attribute_searches = 0U;
    for (const CssSelectorSimpleV1& simple : selector.simple) {
        switch (simple.kind) {
        case CssSelectorSimpleKindV1::Id:
        case CssSelectorSimpleKindV1::Class:
        case CssSelectorSimpleKindV1::AttributeExists:
        case CssSelectorSimpleKindV1::AttributeEquals:
            ++attribute_searches;
            break;
        case CssSelectorSimpleKindV1::Universal:
        case CssSelectorSimpleKindV1::Type:
            break;
        default:
            return false;
        }
    }

    const std::uint64_t simple_units =
        static_cast<std::uint64_t>(selector.simple.size());
    const std::uint64_t semantic_units =
        static_cast<std::uint64_t>(semantic_bytes);
    if (simple_units > config.maximum_work_units ||
        semantic_units > config.maximum_work_units - simple_units) {
        return false;
    }
    std::uint64_t estimated_work = simple_units + semantic_units;
    const std::uint64_t scan_units =
        semantic_units +
        static_cast<std::uint64_t>(node.attributes.size());
    if (attribute_searches > 0U) {
        if (scan_units >
            (config.maximum_work_units - estimated_work) /
                attribute_searches) {
            return false;
        }
        estimated_work += scan_units * attribute_searches;
    }
    if (estimated_work > config.maximum_work_units) {
        return false;
    }

    const auto slice_valid = [&](CssSelectorTextSliceV1 slice) noexcept {
        const std::size_t offset = static_cast<std::size_t>(slice.offset);
        const std::size_t length = static_cast<std::size_t>(slice.length);
        return offset <= selector.text.size() &&
            length <= selector.text.size() - offset;
    };

    for (const CssSelectorSimpleV1& simple : selector.simple) {
        if (!slice_valid(simple.name) || !slice_valid(simple.value)) {
            return false;
        }
        const std::string_view name = selector.resolve(simple.name);
        const std::string_view value = selector.resolve(simple.value);
        if (simple.kind != CssSelectorSimpleKindV1::Universal &&
            simple.name.length == 0U) {
            return false;
        }

        switch (simple.kind) {
        case CssSelectorSimpleKindV1::Universal:
            if (simple.name.length != 0U || simple.value.length != 0U) {
                return false;
            }
            break;
        case CssSelectorSimpleKindV1::Type:
            if (simple.value.length != 0U) {
                return false;
            }
            if (!ascii_iequals(node.tag, name)) {
                return true;
            }
            break;
        case CssSelectorSimpleKindV1::Id: {
            if (simple.value.length != 0U) {
                return false;
            }
            const CssSelectorAttributeV1* attribute =
                find_attribute(node, "id");
            if (attribute == nullptr || attribute->value != name) {
                return true;
            }
            break;
        }
        case CssSelectorSimpleKindV1::Class: {
            if (simple.value.length != 0U) {
                return false;
            }
            const CssSelectorAttributeV1* attribute =
                find_attribute(node, "class");
            if (attribute == nullptr ||
                !class_token_matches(attribute->value, name)) {
                return true;
            }
            break;
        }
        case CssSelectorSimpleKindV1::AttributeExists:
            if (simple.value.length != 0U) {
                return false;
            }
            if (find_attribute(node, name) == nullptr) {
                return true;
            }
            break;
        case CssSelectorSimpleKindV1::AttributeEquals: {
            const CssSelectorAttributeV1* attribute =
                find_attribute(node, name);
            if (attribute == nullptr || attribute->value != value) {
                return true;
            }
            break;
        }
        default:
            return false;
        }
    }

    *matched = true;
    return true;
}

bool match_css_compound_selector_v1(
    const CssCompoundSelectorV1& selector,
    const CssSelectorNodeV1& node,
    bool* matched) noexcept {
    return match_css_compound_selector_v1(
        selector,
        node,
        CssSelectorMatchConfigV1{},
        matched);
}

} // namespace zevryon::style
