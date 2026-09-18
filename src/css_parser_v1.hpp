#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

namespace zevryon::style {

struct CssTextSliceV1 {
    std::uint32_t offset{0U};
    std::uint32_t length{0U};

    bool operator==(const CssTextSliceV1&) const noexcept = default;
};

struct CssDeclarationV1 {
    CssTextSliceV1 property{};
    CssTextSliceV1 value{};
    bool important{false};

    bool operator==(const CssDeclarationV1&) const noexcept = default;
};

struct CssStyleRuleV1 {
    CssTextSliceV1 selector{};
    std::uint32_t declaration_offset{0U};
    std::uint32_t declaration_count{0U};

    bool operator==(const CssStyleRuleV1&) const noexcept = default;
};

enum class CssParserV1ErrorKind : std::uint8_t {
    None = 0,
    InvalidConfiguration,
    InputTooLarge,
    UnsupportedSyntax,
    InvalidSelector,
    InvalidDeclaration,
    UnterminatedComment,
    UnterminatedString,
    UnbalancedBlock,
    NestingLimitExceeded,
    RuleLimitExceeded,
    DeclarationLimitExceeded,
    OutputBudgetExceeded,
    AllocationFailure,
};

struct CssParserV1Error {
    CssParserV1ErrorKind kind{CssParserV1ErrorKind::None};
    std::size_t byte_offset{0U};
    std::string message;
};

struct CssParserV1Config {
    std::size_t maximum_input_bytes{1024U * 1024U};
    std::uint32_t maximum_rules{65'536U};
    std::uint32_t maximum_declarations{262'144U};
    std::uint32_t maximum_nesting_depth{64U};
    std::size_t maximum_output_text_bytes{8U * 1024U * 1024U};

    bool valid() const noexcept;
};

struct CssParserV1Stats {
    std::uint64_t input_bytes{0U};
    std::uint64_t preprocessed_input_bytes{0U};
    std::uint64_t null_replacements{0U};
    std::uint64_t newline_normalizations{0U};
    std::uint64_t invalid_utf8_replacements{0U};
    std::uint64_t rules{0U};
    std::uint64_t declarations{0U};
    std::uint64_t important_declarations{0U};
    std::uint64_t comments{0U};
    std::uint32_t maximum_nesting_depth{0U};
    std::uint64_t output_text_bytes{0U};
};

struct CssStylesheetV1 {
    explicit CssStylesheetV1(std::pmr::memory_resource* resource);

    std::pmr::string text;
    std::pmr::vector<CssStyleRuleV1> rules;
    std::pmr::vector<CssDeclarationV1> declarations;

    std::pmr::memory_resource* resource() const noexcept;
    std::string_view resolve(CssTextSliceV1 slice) const noexcept;
    void release() noexcept;
};

const char* css_parser_v1_error_kind_name(CssParserV1ErrorKind kind) noexcept;

// Strict, bounded CSS syntax foundation. Raw UTF-8 input is preprocessed before
// parsing: NUL becomes U+FFFD, CRLF/CR/FF become LF, and malformed UTF-8 is
// replaced deterministically. The production slice then parses qualified style
// rules and declaration lists, including comments, strings, escapes, balanced
// (), [] and {} value blocks, custom properties and !important. At-rules and
// selector semantics intentionally remain outside this foundation.
bool parse_css_stylesheet_v1(
    std::string_view input,
    CssParserV1Config config,
    CssStylesheetV1* output,
    CssParserV1Stats* stats,
    CssParserV1Error* error) noexcept;

} // namespace zevryon::style
