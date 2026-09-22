#include "css_parser_v1.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <iterator>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include <memory_resource>
#include <string>
#include <string_view>

namespace {

using namespace zevryon::style;

std::string hex(std::string_view value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string output;
    output.reserve(value.size() * 2U);
    for (const char ch : value) {
        const unsigned char byte =
            static_cast<unsigned char>(ch);
        output.push_back(kDigits[(byte >> 4U) & 0x0fU]);
        output.push_back(kDigits[byte & 0x0fU]);
    }
    return output;
}

const char* context_name(CssAtRuleContextV1 context) noexcept {
    switch (context) {
    case CssAtRuleContextV1::TopLevel:
        return "top";
    case CssAtRuleContextV1::DeclarationList:
        return "declaration-list";
    }
    return "unknown";
}

} // namespace

int main() {
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) {
        std::cerr << "failed to put parser probe stdin in binary mode\n";
        return 2;
    }
#endif

    const std::string input{
        std::istreambuf_iterator<char>(std::cin),
        std::istreambuf_iterator<char>()};

    std::pmr::monotonic_buffer_resource memory;
    CssStylesheetV1 sheet(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;
    const bool ok = parse_css_stylesheet_v1(
        input,
        CssParserV1Config{},
        &sheet,
        &stats,
        &error);

    if (!ok) {
        std::cout
            << "STATUS\tERROR\n"
            << "ERROR\t"
            << css_parser_v1_error_kind_name(error.kind)
            << '\t' << error.byte_offset << '\n';
        return 0;
    }

    std::cout << "STATUS\tOK\n";
    std::cout << "RULE_COUNT\t" << sheet.rules.size() << '\n';
    for (std::size_t index = 0U;
         index < sheet.rules.size();
         ++index) {
        const CssStyleRuleV1& rule = sheet.rules[index];
        std::cout
            << "R\t" << index
            << '\t' << hex(sheet.resolve(rule.selector))
            << '\t' << rule.declaration_offset
            << '\t' << rule.declaration_count
            << '\n';
    }

    std::cout
        << "DECL_COUNT\t"
        << sheet.declarations.size()
        << '\n';
    for (std::size_t index = 0U;
         index < sheet.declarations.size();
         ++index) {
        const CssDeclarationV1& declaration =
            sheet.declarations[index];
        std::cout
            << "D\t" << index
            << '\t' << hex(sheet.resolve(declaration.property))
            << '\t' << hex(sheet.resolve(declaration.value))
            << '\t' << (declaration.important ? 1 : 0)
            << '\n';
    }

    std::cout
        << "ATRULE_COUNT\t"
        << sheet.at_rules.size()
        << '\n';
    for (std::size_t index = 0U;
         index < sheet.at_rules.size();
         ++index) {
        const CssAtRuleV1& rule = sheet.at_rules[index];
        std::cout
            << "A\t" << index
            << '\t' << context_name(rule.context)
            << '\t' << rule.owner_rule_index
            << '\t' << (rule.has_block ? 1 : 0)
            << '\t' << hex(sheet.resolve(rule.name))
            << '\t' << hex(sheet.resolve(rule.prelude))
            << '\t' << hex(sheet.resolve(rule.block))
            << '\n';
    }

    std::cout
        << "STATS\t"
        << stats.input_bytes << '\t'
        << stats.preprocessed_input_bytes << '\t'
        << stats.null_replacements << '\t'
        << stats.newline_normalizations << '\t'
        << stats.invalid_utf8_replacements << '\t'
        << stats.rules << '\t'
        << stats.declarations << '\t'
        << stats.important_declarations << '\t'
        << stats.comments << '\t'
        << stats.maximum_nesting_depth << '\t'
        << stats.output_text_bytes << '\t'
        << stats.at_rules << '\t'
        << stats.dropped_charset_rules << '\t'
        << stats.recovered_invalid_declarations
        << '\n';
    return 0;
}
