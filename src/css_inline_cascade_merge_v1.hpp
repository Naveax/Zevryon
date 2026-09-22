#pragma once

#include "css_cascade_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>

namespace zevryon::style {

struct CssInlineCascadeMergeConfigV1 {
    static constexpr std::size_t kMaximumAuthorPropertiesLimit = 1'048'576U;
    static constexpr std::size_t kMaximumInlineDeclarationsLimit = 1'048'576U;
    static constexpr std::size_t kMaximumOutputPropertiesLimit = 1'048'576U;
    static constexpr std::size_t kMaximumOutputTextBytesLimit =
        128U * 1024U * 1024U;
    static constexpr std::uint64_t kMaximumWorkUnitsLimit =
        64U * 1024U * 1024U;

    std::size_t maximum_author_properties{65'536U};
    std::size_t maximum_inline_declarations{65'536U};
    std::size_t maximum_output_properties{65'536U};
    std::size_t maximum_output_text_bytes{8U * 1024U * 1024U};
    std::uint64_t maximum_work_units{8U * 1024U * 1024U};

    bool valid() const noexcept;
};

enum class CssInlineCascadeMergeErrorKindV1 : std::uint8_t {
    None = 0,
    InvalidConfiguration,
    AuthorPropertyLimitExceeded,
    InlineDeclarationLimitExceeded,
    OutputPropertyLimitExceeded,
    TextBudgetExceeded,
    WorkBudgetExceeded,
    InvalidAuthorCascade,
    InvalidInlineDeclaration,
    RepresentationOverflow,
    AllocationFailure,
};

struct CssInlineCascadeMergeErrorV1 {
    CssInlineCascadeMergeErrorKindV1 kind{
        CssInlineCascadeMergeErrorKindV1::None};
    std::size_t index{0U};
    std::string message;
};

struct CssInlineCascadeMergeStatsV1 {
    std::uint64_t author_properties{0U};
    std::uint64_t inline_declarations{0U};
    std::uint64_t inline_duplicate_declarations{0U};
    std::uint64_t inline_duplicate_replacements{0U};
    std::uint64_t inline_overrides_author{0U};
    std::uint64_t author_important_preserved{0U};
    std::uint64_t output_properties{0U};
    std::uint64_t output_text_bytes{0U};
    std::uint64_t work_units{0U};
};

// Final author-origin winner set backed by its own text storage. The cascade
// provenance fields are intentionally not authoritative; this structure exists
// only to feed intern_css_cascade_style_v1 after precedence is resolved.
struct CssInlineMergedCascadeV1 {
    explicit CssInlineMergedCascadeV1(
        std::pmr::memory_resource* memory);

    CssStylesheetV1 stylesheet;
    CssCascadeResultV1 cascade;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;
};

const char* css_inline_cascade_merge_error_kind_name_v1(
    CssInlineCascadeMergeErrorKindV1 kind) noexcept;

// Merges an already-computed author stylesheet cascade with a parsed inline
// declaration list from the same author origin.
//
// Current profile semantics:
// - !important beats non-important;
// - at equal importance inline declarations beat author stylesheet winners;
// - duplicate inline declarations resolve by importance, then later source
//   order;
// - normal property names rely on parser canonicalization;
// - custom properties remain case-sensitive.
//
// inline_declarations must borrow property/value slices from inline_stylesheet.
bool merge_css_author_and_inline_cascade_v1(
    const CssStylesheetV1& author_stylesheet,
    const CssCascadeResultV1& author_cascade,
    const CssStylesheetV1& inline_stylesheet,
    std::span<const CssDeclarationV1> inline_declarations,
    CssInlineCascadeMergeConfigV1 config,
    CssInlineMergedCascadeV1* output,
    CssInlineCascadeMergeStatsV1* stats,
    CssInlineCascadeMergeErrorV1* error) noexcept;

} // namespace zevryon::style
