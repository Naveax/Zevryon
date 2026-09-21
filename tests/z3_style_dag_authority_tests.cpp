#include "css_style_dag_v1.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

namespace {

using zevryon::core::LedgerMemoryResource;
using zevryon::core::ResourceClass;
using zevryon::core::ResourceLedger;
using namespace zevryon::style;

constexpr std::size_t kUniqueStyles = 32U;
constexpr std::size_t kPropertiesPerStyle = 8U;
constexpr std::size_t kTotalRequests = 4096U;
constexpr std::size_t kExpectedNodes =
    1U + 7U + kUniqueStyles;
constexpr std::size_t kDagLedgerHardLimit =
    1U * 1024U * 1024U;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: Z3 style DAG authority: "
                  << message << '\n';
        return false;
    }
    return true;
}

std::string build_stylesheet() {
    std::string css;
    css.reserve(8192U);
    for (std::size_t index = 0U;
         index < kUniqueStyles;
         ++index) {
        css += ".s";
        css += std::to_string(index);
        css += "{";
        if ((index % 2U) == 0U) {
            css += "a:1;b:2;c:3;d:4;e:5;f:6;g:7;z:";
            css += std::to_string(index);
            css += ";";
        } else {
            css += "z:";
            css += std::to_string(index);
            css += ";g:7;f:6;e:5;d:4;c:3;b:2;a:1;";
        }
        css += "}";
    }
    return css;
}

bool parse_authority_sheet(
    const std::string& css,
    CssStylesheetV1* sheet) {
    CssParserV1Stats stats;
    CssParserV1Error error;
    return require(
        parse_css_stylesheet_v1(
            css,
            CssParserV1Config{},
            sheet,
            &stats,
            &error),
        "production stylesheet parser must accept authority corpus") &&
        require(
            sheet->rules.size() == kUniqueStyles,
            "authority stylesheet must contain exactly 32 rules") &&
        require(
            sheet->declarations.size() ==
                kUniqueStyles * kPropertiesPerStyle,
            "authority stylesheet must contain exactly 256 declarations");
}

bool cascade_style(
    const CssStylesheetV1& sheet,
    std::size_t style_index,
    CssCascadeResultV1* cascade) {
    const std::string class_name =
        "s" + std::to_string(style_index);
    const std::array<CssSelectorAttributeV1, 1> attributes{{
        {"class", class_name},
    }};
    const CssSelectorNodeV1 node{"div", attributes};

    CssCascadeStatsV1 stats;
    CssCascadeErrorV1 error;
    if (!cascade_css_author_rules_v1(
            sheet,
            node,
            CssCascadeConfigV1{},
            cascade,
            &stats,
            &error)) {
        return require(false, "production cascade must resolve authority style");
    }
    return require(
        cascade->winners.size() == kPropertiesPerStyle,
        "each authority style must resolve exactly eight winning properties");
}

bool run_reuse_authority(
    const CssStylesheetV1& sheet,
    CssComputedStyleDagV1* dag) {
    CssCascadeResultV1 cascade(std::pmr::new_delete_resource());
    std::array<std::uint32_t, kUniqueStyles> terminals{};
    terminals.fill(kCssStyleDagNoNodeV1);

    for (std::size_t request = 0U;
         request < kTotalRequests;
         ++request) {
        const std::size_t style_index =
            request % kUniqueStyles;
        cascade.release();
        if (!cascade_style(sheet, style_index, &cascade)) {
            return false;
        }

        CssStyleDagStatsV1 stats;
        CssStyleDagErrorV1 error;
        std::uint32_t terminal = kCssStyleDagNoNodeV1;
        if (!intern_css_cascade_style_v1(
                sheet,
                cascade,
                CssStyleDagConfigV1{},
                dag,
                &terminal,
                &stats,
                &error)) {
            return require(false, "production DAG interning must succeed");
        }
        if (!require(
                terminal != kCssStyleDagNoNodeV1,
                "successful intern must return a terminal node")) {
            return false;
        }

        if (request < kUniqueStyles) {
            if (!require(
                    terminals[style_index] ==
                        kCssStyleDagNoNodeV1,
                    "first authority cycle must introduce each style once")) {
                return false;
            }
            terminals[style_index] = terminal;
        } else if (!require(
                       terminal == terminals[style_index],
                       "repeated computed style must reuse exact terminal identity")) {
            return false;
        }

        if (request + 1U == kUniqueStyles) {
            if (!require(
                    dag->nodes.size() == kExpectedNodes,
                    "32 unique styles must retain exactly 40 DAG nodes")) {
                return false;
            }
        }
        if (request >= kUniqueStyles) {
            if (!require(
                    stats.nodes_created == 0U,
                    "repeated style request must create zero nodes")) {
                return false;
            }
            if (!require(
                    dag->nodes.size() == kExpectedNodes,
                    "repeated style requests must not grow retained DAG")) {
                return false;
            }
        }
    }

    if (!require(
            dag->nodes.size() == kExpectedNodes,
            "4096 requests must finish at exactly 40 retained nodes")) {
        return false;
    }
    for (std::size_t index = 0U;
         index < terminals.size();
         ++index) {
        if (!require(
                terminals[index] != kCssStyleDagNoNodeV1,
                "every authority style must have a terminal identity")) {
            return false;
        }
        if (!require(
                dag->nodes[terminals[index]].depth ==
                    kPropertiesPerStyle,
                "every terminal must have exact depth eight")) {
            return false;
        }
    }
    return true;
}

bool run_node_limit_authority(const CssStylesheetV1& sheet) {
    ResourceLedger ledger;
    ledger.set_hard_limit(
        ResourceClass::ComputedStyle,
        kDagLedgerHardLimit);
    LedgerMemoryResource memory(
        ledger,
        ResourceClass::ComputedStyle);
    CssComputedStyleDagV1 dag(&memory);

    CssCascadeResultV1 cascade(std::pmr::new_delete_resource());

    CssStyleDagConfigV1 config;
    config.maximum_nodes = kExpectedNodes - 1U;

    for (std::size_t style_index = 0U;
         style_index < kUniqueStyles;
         ++style_index) {
        cascade.release();
        if (!cascade_style(sheet, style_index, &cascade)) {
            return false;
        }

        CssStyleDagStatsV1 stats;
        CssStyleDagErrorV1 error;
        std::uint32_t terminal = kCssStyleDagNoNodeV1;
        const bool accepted = intern_css_cascade_style_v1(
            sheet,
            cascade,
            config,
            &dag,
            &terminal,
            &stats,
            &error);

        if (style_index + 1U < kUniqueStyles) {
            if (!require(
                    accepted,
                    "node-limited authority must accept first 31 unique styles")) {
                return false;
            }
            continue;
        }

        if (!require(
                !accepted,
                "40th retained node must be rejected by 39-node ceiling")) {
            return false;
        }
        if (!require(
                error.kind ==
                    CssStyleDagErrorKindV1::NodeLimitExceeded,
                "node ceiling rejection must report node-limit-exceeded")) {
            return false;
        }
        if (!require(
                terminal == kCssStyleDagNoNodeV1,
                "failed node-limited intern must not publish terminal")) {
            return false;
        }
        if (!require(
                dag.nodes.size() == kExpectedNodes - 1U,
                "failed node-limited extension must preserve 39 logical nodes")) {
            return false;
        }
    }

    return require(
        ledger.snapshot(ResourceClass::ComputedStyle)
                .rejected_reservations == 0U,
        "logical node limit must reject before exhausting ledger");
}

} // namespace

int main() {
    std::pmr::monotonic_buffer_resource sheet_memory;
    CssStylesheetV1 sheet(&sheet_memory);
    const std::string css = build_stylesheet();
    if (!parse_authority_sheet(css, &sheet)) {
        return 1;
    }

    ResourceLedger ledger;
    ledger.set_hard_limit(
        ResourceClass::ComputedStyle,
        kDagLedgerHardLimit);
    LedgerMemoryResource memory(
        ledger,
        ResourceClass::ComputedStyle);
    CssComputedStyleDagV1 dag(&memory);

    if (!run_reuse_authority(sheet, &dag)) {
        return 1;
    }
    const auto snapshot =
        ledger.snapshot(ResourceClass::ComputedStyle);
    if (!require(
            snapshot.current_bytes <= kDagLedgerHardLimit,
            "retained DAG must remain within 1 MiB ComputedStyle ledger")) {
        return 1;
    }
    if (!require(
            snapshot.rejected_reservations == 0U,
            "reuse authority must not hit ledger rejection")) {
        return 1;
    }

    if (!run_node_limit_authority(sheet)) {
        return 1;
    }

    std::cout
        << "{\"schema\":\"zevryon.z3-style-dag-authority.v1\","
        << "\"unique_styles\":" << kUniqueStyles << ','
        << "\"properties_per_style\":" << kPropertiesPerStyle << ','
        << "\"requests\":" << kTotalRequests << ','
        << "\"retained_nodes\":" << dag.nodes.size() << ','
        << "\"ledger_hard_limit_bytes\":" << kDagLedgerHardLimit
        << "}\n";
    return 0;
}
