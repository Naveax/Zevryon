#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zevryon::style {

enum class CssSelectorSimpleKindV1 : std::uint8_t {
    Universal = 0,
    Type,
    Id,
    Class,
    AttributeExists,
    AttributeEquals,
};

struct CssSelectorTextSliceV1 {
    std::uint32_t offset{0U};
    std::uint32_t length{0U};
};

struct CssSelectorSimpleV1 {
    CssSelectorSimpleKindV1 kind{CssSelectorSimpleKindV1::Universal};
    CssSelectorTextSliceV1 name{};
    CssSelectorTextSliceV1 value{};
};

struct CssSpecificityV1 {
    std::uint32_t ids{0U};
    std::uint32_t classes{0U};
    std::uint32_t types{0U};

    bool operator==(const CssSpecificityV1&) const noexcept = default;
};

int compare_css_specificity_v1(
    CssSpecificityV1 left,
    CssSpecificityV1 right) noexcept;

struct CssCompoundSelectorV1 {
    explicit CssCompoundSelectorV1(std::pmr::memory_resource* memory);

    std::pmr::string text;
    std::pmr::vector<CssSelectorSimpleV1> simple;
    CssSpecificityV1 specificity{};

    std::pmr::memory_resource* resource() const noexcept;
    std::string_view resolve(CssSelectorTextSliceV1 slice) const noexcept;
    void release() noexcept;
};

struct CssSelectorCompileConfigV1 {
    static constexpr std::size_t kMaximumInputBytesLimit = 1024U * 1024U;
    static constexpr std::size_t kMaximumSimpleSelectorsLimit = 65'536U;
    static constexpr std::size_t kMaximumOutputTextBytesLimit = 4U * 1024U * 1024U;

    std::size_t maximum_input_bytes{64U * 1024U};
    std::size_t maximum_simple_selectors{256U};
    std::size_t maximum_output_text_bytes{256U * 1024U};

    bool valid() const noexcept;
};

enum class CssSelectorCompileErrorKindV1 : std::uint8_t {
    None = 0,
    InvalidConfiguration,
    EmptySelector,
    InputTooLarge,
    UnsupportedSyntax,
    InvalidIdentifier,
    InvalidAttribute,
    SimpleSelectorLimitExceeded,
    OutputBudgetExceeded,
    SpecificityOverflow,
    AllocationFailure,
};

struct CssSelectorCompileErrorV1 {
    CssSelectorCompileErrorKindV1 kind{CssSelectorCompileErrorKindV1::None};
    std::size_t byte_offset{0U};
    std::string message;
};

struct CssSelectorCompileStatsV1 {
    std::uint64_t input_bytes{0U};
    std::uint64_t simple_selectors{0U};
    std::uint64_t universal_selectors{0U};
    std::uint64_t type_selectors{0U};
    std::uint64_t id_selectors{0U};
    std::uint64_t class_selectors{0U};
    std::uint64_t attribute_selectors{0U};
};

const char* css_selector_compile_error_kind_name_v1(
    CssSelectorCompileErrorKindV1 kind) noexcept;

bool compile_css_compound_selector_v1(
    std::string_view input,
    CssSelectorCompileConfigV1 config,
    CssCompoundSelectorV1* output,
    CssSelectorCompileStatsV1* stats,
    CssSelectorCompileErrorV1* error) noexcept;

struct CssSelectorAttributeV1 {
    std::string_view name;
    std::string_view value;
};

struct CssSelectorNodeV1 {
    std::string_view tag;
    std::span<const CssSelectorAttributeV1> attributes;
};

struct CssSelectorMatchConfigV1 {
    static constexpr std::size_t kMaximumAttributesLimit = 65'536U;
    static constexpr std::size_t kMaximumSemanticBytesLimit =
        16U * 1024U * 1024U;
    static constexpr std::uint64_t kMaximumWorkUnitsLimit =
        64U * 1024U * 1024U;

    std::size_t maximum_attributes{4096U};
    std::size_t maximum_semantic_bytes{1U * 1024U * 1024U};
    std::uint64_t maximum_work_units{8U * 1024U * 1024U};

    bool valid() const noexcept;
};

bool match_css_compound_selector_v1(
    const CssCompoundSelectorV1& selector,
    const CssSelectorNodeV1& node,
    CssSelectorMatchConfigV1 config,
    bool* matched) noexcept;

bool match_css_compound_selector_v1(
    const CssCompoundSelectorV1& selector,
    const CssSelectorNodeV1& node,
    bool* matched) noexcept;

} // namespace zevryon::style
