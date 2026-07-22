#include "bidi_resolver.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"
#include "unicode_bidi.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory_resource>
#include <string>
#include <vector>

namespace {

using zevryon::text::BidiClass;
using zevryon::text::BidiError;
using zevryon::text::BidiErrorKind;
using zevryon::text::BidiStats;
using zevryon::text::DecodedCodePoint;
using zevryon::text::ParagraphDirection;

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

struct Result {
    bool success{false};
    std::vector<std::uint8_t> levels;
    std::vector<std::uint32_t> order;
    BidiStats stats;
    BidiError error;
    zevryon::core::ResourceSnapshot resource;
};

Result resolve_classes(
    std::initializer_list<BidiClass> classes,
    ParagraphDirection direction,
    std::size_t budget = 1U << 20U) {
    const std::vector<BidiClass> input(classes);
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(zevryon::core::ResourceClass::BidiBuffer, budget);
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiBuffer);
    std::pmr::vector<std::uint8_t> levels(&memory);
    std::pmr::vector<std::uint32_t> order(&memory);
    Result result;
    result.success = zevryon::text::resolve_bidi_classes(
        input,
        direction,
        &levels,
        &order,
        &result.stats,
        &result.error);
    result.levels.assign(levels.begin(), levels.end());
    result.order.assign(order.begin(), order.end());
    result.resource = ledger.snapshot(zevryon::core::ResourceClass::BidiBuffer);
    return result;
}

std::uint8_t utf8_length(std::uint32_t value) noexcept {
    return value <= 0x7fU ? 1U
         : value <= 0x7ffU ? 2U
         : value <= 0xffffU ? 3U
         : 4U;
}

Result resolve_codepoints(
    std::initializer_list<std::uint32_t> values,
    ParagraphDirection direction,
    std::size_t budget = 1U << 20U) {
    std::vector<DecodedCodePoint> input;
    input.reserve(values.size());
    std::uint64_t source = 0U;
    for (const std::uint32_t value : values) {
        const std::uint8_t length = utf8_length(value);
        input.emplace_back(value, source, source + length, false);
        source += length;
    }
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(zevryon::core::ResourceClass::BidiBuffer, budget);
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiBuffer);
    std::pmr::vector<std::uint8_t> levels(&memory);
    std::pmr::vector<std::uint32_t> order(&memory);
    Result result;
    result.success = zevryon::text::resolve_bidi_codepoints(
        input,
        direction,
        &levels,
        &order,
        &result.stats,
        &result.error);
    result.levels.assign(levels.begin(), levels.end());
    result.order.assign(order.begin(), order.end());
    result.resource = ledger.snapshot(zevryon::core::ResourceClass::BidiBuffer);
    return result;
}

bool expect(
    const Result& result,
    std::initializer_list<std::uint8_t> levels,
    std::initializer_list<std::uint32_t> order,
    const std::string& label) {
    return require(result.success, label + " succeeds") &&
           require(result.levels == std::vector<std::uint8_t>(levels), label + " levels") &&
           require(result.order == std::vector<std::uint32_t>(order), label + " order") &&
           require(result.resource.accounting_errors == 0U, label + " accounting clean") &&
           require(
               result.resource.peak_bytes <= result.resource.hard_limit_bytes,
               label + " hard budget");
}

bool test_properties() {
    const auto open = zevryon::text::bidi_bracket_of('(');
    const auto close = zevryon::text::bidi_bracket_of(')');
    std::uint32_t mirror = 0U;
    BidiClass parsed = BidiClass::L;
    return require(zevryon::text::bidi_class_of('A') == BidiClass::L, "Latin bidi class") &&
           require(zevryon::text::bidi_class_of(0x05d0U) == BidiClass::R, "Hebrew bidi class") &&
           require(zevryon::text::bidi_class_of(0x0627U) == BidiClass::AL, "Arabic bidi class") &&
           require(open.type == zevryon::text::BidiBracketType::Open, "open bracket type") &&
           require(open.paired_codepoint == ')', "open bracket pair") &&
           require(close.type == zevryon::text::BidiBracketType::Close, "close bracket type") &&
           require(zevryon::text::bidi_mirror_of('(', &mirror) && mirror == ')', "mirror lookup") &&
           require(
               zevryon::text::bidi_class_short_name(BidiClass::RLI) == "RLI",
               "short class name") &&
           require(
               zevryon::text::bidi_class_from_name("Right_To_Left_Isolate", &parsed) &&
                   parsed == BidiClass::RLI,
               "long class parse");
}

bool test_basic_rules() {
    return expect(
               resolve_classes(
                   {BidiClass::L, BidiClass::R, BidiClass::EN},
                   ParagraphDirection::Auto),
               {0U, 1U, 2U},
               {0U, 2U, 1U},
               "basic implicit and L2") &&
           expect(
               resolve_classes(
                   {BidiClass::R, BidiClass::NSM, BidiClass::WS, BidiClass::R},
                   ParagraphDirection::LeftToRight),
               {1U, 1U, 1U, 1U},
               {3U, 2U, 1U, 0U},
               "W1 and neutral context") &&
           expect(
               resolve_classes(
                   {BidiClass::AL, BidiClass::EN, BidiClass::CS, BidiClass::EN},
                   ParagraphDirection::LeftToRight),
               {1U, 2U, 2U, 2U},
               {1U, 2U, 3U, 0U},
               "W2 through W7");
}

bool test_explicit_and_isolates() {
    return expect(
               resolve_classes(
                   {BidiClass::L, BidiClass::RLO, BidiClass::L, BidiClass::PDF, BidiClass::L},
                   ParagraphDirection::LeftToRight),
               {0U, zevryon::text::kBidiRemovedLevel, 1U,
                zevryon::text::kBidiRemovedLevel, 0U},
               {0U, 2U, 4U},
               "explicit override and X9") &&
           expect(
               resolve_classes(
                   {BidiClass::L, BidiClass::RLI, BidiClass::R,
                    BidiClass::PDI, BidiClass::L},
                   ParagraphDirection::LeftToRight),
               {0U, 0U, 1U, 0U, 0U},
               {0U, 1U, 2U, 3U, 4U},
               "isolating run sequence") &&
           expect(
               resolve_classes(
                   {BidiClass::FSI, BidiClass::R, BidiClass::PDI},
                   ParagraphDirection::LeftToRight),
               {0U, 1U, 0U},
               {0U, 1U, 2U},
               "FSI resolves from isolate content");
}

bool test_brackets() {
    return expect(
               resolve_codepoints(
                   {'A', '(', 0x05d0U, ')', 'B'},
                   ParagraphDirection::LeftToRight),
               {0U, 0U, 1U, 0U, 0U},
               {0U, 1U, 2U, 3U, 4U},
               "N0 embedding-direction brackets") &&
           expect(
               resolve_codepoints(
                   {0x05d0U, '(', 'A', ')', 0x05d1U},
                   ParagraphDirection::RightToLeft),
               {1U, 1U, 2U, 1U, 1U},
               {4U, 3U, 2U, 1U, 0U},
               "N0 opposite-direction brackets");
}

bool test_l1_and_empty() {
    return expect(
               resolve_classes(
                   {BidiClass::R, BidiClass::WS, BidiClass::B},
                   ParagraphDirection::LeftToRight),
               {1U, 0U, 0U},
               {0U, 1U, 2U},
               "L1 paragraph reset") &&
           expect(
               resolve_classes({}, ParagraphDirection::Auto),
               {},
               {},
               "empty paragraph");
}

bool test_budget_and_invalid_input() {
    const Result budget = resolve_classes(
        {BidiClass::L, BidiClass::R, BidiClass::L},
        ParagraphDirection::Auto,
        1U);
    if (!require(!budget.success, "hard budget rejects") ||
        !require(
            budget.error.kind == BidiErrorKind::OutputBudgetExceeded,
            "hard budget error kind") ||
        !require(budget.resource.rejected_reservations > 0U, "budget rejection counted")) {
        return false;
    }

    std::vector<DecodedCodePoint> invalid;
    invalid.emplace_back('a', 0U, 1U, false);
    invalid.emplace_back('b', 2U, 3U, false);
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(zevryon::core::ResourceClass::BidiBuffer, 4096U);
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiBuffer);
    std::pmr::vector<std::uint8_t> levels(&memory);
    std::pmr::vector<std::uint32_t> order(&memory);
    BidiStats stats;
    BidiError error;
    return require(
               !zevryon::text::resolve_bidi_codepoints(
                   invalid,
                   ParagraphDirection::Auto,
                   &levels,
                   &order,
                   &stats,
                   &error),
               "discontinuous source rejected") &&
           require(error.kind == BidiErrorKind::InvalidInput, "invalid source error kind");
}

} // namespace

int main() {
    if (!test_properties() ||
        !test_basic_rules() ||
        !test_explicit_and_isolates() ||
        !test_brackets() ||
        !test_l1_and_empty() ||
        !test_budget_and_invalid_input()) {
        return 1;
    }
    std::cout << "bidi resolver tests passed\n";
    return 0;
}
