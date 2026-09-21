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

constexpr std::size_t kFanoutStyles = 512U;
constexpr std::size_t kExpectedRetainedNodes = 515U;
constexpr std::size_t kExpectedRetainedTextBytes = 3880U;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: Z3 bounded style DAG authority: "
                  << message << '\n';
        return false;
    }
    return true;
}

bool parse_sheet(
    std::string_view source,
    CssStylesheetV1* sheet) {
    CssParserV1Stats stats;
    CssParserV1Error error;
    return parse_css_stylesheet_v1(
        source,
        CssParserV1Config{},
        sheet,
        &stats,
        &error);
}

bool cascade_sheet(
    const CssStylesheetV1& sheet,
    const CssSelectorNodeV1& node,
    CssCascadeResultV1* result) {
    CssCascadeStatsV1 stats;
    CssCascadeErrorV1 error;
    return cascade_css_author_rules_v1(
        sheet,
        node,
        CssCascadeConfigV1{},
        result,
        &stats,
        &error);
}

bool intern_style(
    const CssStylesheetV1& sheet,
    const CssCascadeResultV1& cascade,
    CssComputedStyleDagV1* dag,
    std::uint32_t* terminal,
    CssStyleDagStatsV1* stats,
    CssStyleDagErrorV1* error,
    CssStyleDagConfigV1 config = {}) {
    return intern_css_cascade_style_v1(
        sheet,
        cascade,
        config,
        dag,
        terminal,
        stats,
        error);
}

std::string style_source(std::size_t index) {
    const std::string suffix = std::to_string(index);
    return "div{z" + suffix + ":v" + suffix + ";b:2;a:1;}";
}

std::string replay_source(std::size_t index) {
    const std::string suffix = std::to_string(index);
    return "div{z" + suffix + ":wrong;a:1;b:2;}"
        " .card{z" + suffix + ":v" + suffix + "!important;}";
}

bool test_exact_fanout_and_provenance_replay() {
    ResourceLedger ledger;
    ledger.set_hard_limit(
        ResourceClass::ComputedStyle,
        8U * 1024U * 1024U);
    LedgerMemoryResource memory(
        ledger,
        ResourceClass::ComputedStyle);
    CssComputedStyleDagV1 dag(&memory);
    std::vector<std::uint32_t> terminals;
    terminals.reserve(kFanoutStyles);

    for (std::size_t index = 0U;
         index < kFanoutStyles;
         ++index) {
        std::pmr::monotonic_buffer_resource scratch;
        CssStylesheetV1 sheet(&scratch);
        CssCascadeResultV1 cascade(&scratch);
        CssStyleDagStatsV1 stats;
        CssStyleDagErrorV1 error;

        const std::string source = style_source(index);
        if (!require(
                parse_sheet(source, &sheet),
                "fanout stylesheet must parse")) {
            return false;
        }
        if (!require(
                cascade_sheet(
                    sheet,
                    CssSelectorNodeV1{"div", {}},
                    &cascade),
                "fanout cascade must succeed")) {
            return false;
        }

        std::uint32_t terminal = kCssStyleDagNoNodeV1;
        if (!require(
                intern_style(
                    sheet,
                    cascade,
                    &dag,
                    &terminal,
                    &stats,
                    &error),
                "fanout style must intern")) {
            return false;
        }

        const std::uint32_t expected_terminal =
            static_cast<std::uint32_t>(index + 3U);
        if (!require(
                terminal == expected_terminal,
                "fanout terminal identity must be deterministic")) {
            return false;
        }
        if (!require(
                dag.nodes[terminal].depth == 3U,
                "fanout terminal depth must be three")) {
            return false;
        }

        if (index == 0U) {
            if (!require(
                    stats.nodes_created == 4U &&
                        stats.nodes_reused == 0U,
                    "first fanout style must create root plus three nodes")) {
                return false;
            }
        } else {
            if (!require(
                    stats.nodes_created == 1U &&
                        stats.nodes_reused == 2U,
                    "later fanout styles must share the two-node prefix")) {
                return false;
            }
        }
        terminals.push_back(terminal);

        std::pmr::monotonic_buffer_resource replay_scratch;
        CssStylesheetV1 replay_sheet(&replay_scratch);
        CssCascadeResultV1 replay_cascade(&replay_scratch);
        const std::string replay = replay_source(index);
        if (!require(
                parse_sheet(replay, &replay_sheet),
                "provenance replay stylesheet must parse")) {
            return false;
        }
        const std::array<CssSelectorAttributeV1, 1> attributes{{
            {"class", "card"},
        }};
        if (!require(
                cascade_sheet(
                    replay_sheet,
                    CssSelectorNodeV1{"div", attributes},
                    &replay_cascade),
                "provenance replay cascade must succeed")) {
            return false;
        }

        std::uint32_t replay_terminal =
            kCssStyleDagNoNodeV1;
        if (!require(
                intern_style(
                    replay_sheet,
                    replay_cascade,
                    &dag,
                    &replay_terminal,
                    &stats,
                    &error),
                "provenance replay style must intern")) {
            return false;
        }
        if (!require(
                replay_terminal == terminal,
                "equivalent final style must reuse terminal across cascade provenance")) {
            return false;
        }
        if (!require(
                stats.nodes_created == 0U &&
                    stats.nodes_reused == 3U &&
                    stats.text_bytes_appended == 0U,
                "provenance replay must add no DAG storage")) {
            return false;
        }
    }

    if (!require(
            dag.nodes.size() == kExpectedRetainedNodes,
            "exact retained node denominator must match")) {
        return false;
    }
    if (!require(
            dag.text.size() == kExpectedRetainedTextBytes,
            "exact retained text denominator must match")) {
        return false;
    }
    if (!require(
            terminals.front() == 3U &&
                terminals.back() ==
                    static_cast<std::uint32_t>(
                        kExpectedRetainedNodes - 1U),
            "terminal identity range must be exact")) {
        return false;
    }

    for (std::size_t index = 1U;
         index < terminals.size();
         ++index) {
        if (!require(
                terminals[index] > terminals[index - 1U],
                "fanout terminal identities must remain strictly increasing")) {
            return false;
        }
    }

    dag.release();
    if (!require(
            ledger.accounting_clean(),
            "DAG release must return ComputedStyle ledger accounting to clean state")) {
        return false;
    }
    return true;
}

bool test_authority_boundary_failures() {
    std::pmr::monotonic_buffer_resource scratch;
    CssStylesheetV1 baseline_sheet(&scratch);
    CssStylesheetV1 extension_sheet(&scratch);
    CssCascadeResultV1 baseline_cascade(&scratch);
    CssCascadeResultV1 extension_cascade(&scratch);

    if (!require(
            parse_sheet("div{a:1;b:2;}", &baseline_sheet) &&
                parse_sheet(
                    "div{a:1;b:2;c:3;d:4;}",
                    &extension_sheet),
            "authority boundary stylesheets must parse")) {
        return false;
    }
    const CssSelectorNodeV1 node{"div", {}};
    if (!require(
            cascade_sheet(
                baseline_sheet,
                node,
                &baseline_cascade) &&
                cascade_sheet(
                    extension_sheet,
                    node,
                    &extension_cascade),
            "authority boundary cascades must succeed")) {
        return false;
    }

    ResourceLedger ledger;
    ledger.set_hard_limit(
        ResourceClass::ComputedStyle,
        1U * 1024U * 1024U);
    LedgerMemoryResource memory(
        ledger,
        ResourceClass::ComputedStyle);
    CssComputedStyleDagV1 dag(&memory);
    CssStyleDagStatsV1 stats;
    CssStyleDagErrorV1 error;
    std::uint32_t terminal = kCssStyleDagNoNodeV1;

    if (!require(
            intern_style(
                baseline_sheet,
                baseline_cascade,
                &dag,
                &terminal,
                &stats,
                &error),
            "authority boundary baseline must intern")) {
        return false;
    }
    const std::size_t baseline_nodes = dag.nodes.size();
    const std::size_t baseline_text = dag.text.size();

    CssStyleDagConfigV1 config;
    config.maximum_nodes = baseline_nodes + 1U;
    if (!require(
            !intern_style(
                extension_sheet,
                extension_cascade,
                &dag,
                &terminal,
                &stats,
                &error,
                config) &&
                error.kind ==
                    CssStyleDagErrorKindV1::NodeLimitExceeded,
            "node limit must reject partial extension")) {
        return false;
    }
    if (!require(
            dag.nodes.size() == baseline_nodes &&
                dag.text.size() == baseline_text,
            "node-limit rejection must roll logical DAG state back")) {
        return false;
    }

    config = CssStyleDagConfigV1{};
    config.maximum_text_bytes = baseline_text + 2U;
    if (!require(
            !intern_style(
                extension_sheet,
                extension_cascade,
                &dag,
                &terminal,
                &stats,
                &error,
                config) &&
                error.kind ==
                    CssStyleDagErrorKindV1::TextBudgetExceeded,
            "text limit must reject partial extension")) {
        return false;
    }
    if (!require(
            dag.nodes.size() == baseline_nodes &&
                dag.text.size() == baseline_text,
            "text-limit rejection must roll logical DAG state back")) {
        return false;
    }

    config = CssStyleDagConfigV1{};
    config.maximum_properties_per_style = 3U;
    if (!require(
            !intern_style(
                extension_sheet,
                extension_cascade,
                &dag,
                &terminal,
                &stats,
                &error,
                config) &&
                error.kind ==
                    CssStyleDagErrorKindV1::PropertyLimitExceeded,
            "property-count limit must fail closed")) {
        return false;
    }

    config = CssStyleDagConfigV1{};
    config.maximum_style_semantic_bytes = 6U;
    if (!require(
            !intern_style(
                extension_sheet,
                extension_cascade,
                &dag,
                &terminal,
                &stats,
                &error,
                config) &&
                error.kind ==
                    CssStyleDagErrorKindV1::SemanticBudgetExceeded,
            "style semantic-byte limit must fail closed")) {
        return false;
    }

    config = CssStyleDagConfigV1{};
    config.maximum_work_units = 1U;
    if (!require(
            !intern_style(
                extension_sheet,
                extension_cascade,
                &dag,
                &terminal,
                &stats,
                &error,
                config) &&
                error.kind ==
                    CssStyleDagErrorKindV1::WorkBudgetExceeded,
            "work budget must fail closed")) {
        return false;
    }

    const CssStyleDagNodeV1 saved = dag.nodes[2U];
    dag.nodes[2U].parent = 2U;
    config = CssStyleDagConfigV1{};
    if (!require(
            !intern_style(
                baseline_sheet,
                baseline_cascade,
                &dag,
                &terminal,
                &stats,
                &error,
                config) &&
                error.kind ==
                    CssStyleDagErrorKindV1::CorruptDag,
            "corrupt retained topology must fail closed")) {
        return false;
    }
    dag.nodes[2U] = saved;

    dag.release();
    if (!require(
            ledger.accounting_clean(),
            "boundary DAG release must leave clean accounting")) {
        return false;
    }

    ResourceLedger tiny_ledger;
    tiny_ledger.set_hard_limit(
        ResourceClass::ComputedStyle,
        1U);
    LedgerMemoryResource tiny_memory(
        tiny_ledger,
        ResourceClass::ComputedStyle);
    CssComputedStyleDagV1 tiny_dag(&tiny_memory);
    config = CssStyleDagConfigV1{};
    if (!require(
            !intern_style(
                baseline_sheet,
                baseline_cascade,
                &tiny_dag,
                &terminal,
                &stats,
                &error,
                config) &&
                error.kind ==
                    CssStyleDagErrorKindV1::AllocationFailure,
            "real ComputedStyle hard-limit rejection must fail closed")) {
        return false;
    }
    if (!require(
            tiny_ledger
                    .snapshot(ResourceClass::ComputedStyle)
                    .rejected_reservations > 0U &&
                tiny_ledger.accounting_clean(),
            "ledger rejection must be observable and accounting-clean")) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!test_exact_fanout_and_provenance_replay() ||
        !test_authority_boundary_failures()) {
        return 1;
    }

    std::cout
        << "Z3 bounded style DAG authority PASS fanout="
        << kFanoutStyles
        << " provenance_replays=" << kFanoutStyles
        << " retained_nodes=" << kExpectedRetainedNodes
        << " retained_text_bytes=" << kExpectedRetainedTextBytes
        << '\n';
    return 0;
}
