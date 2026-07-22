#include "grapheme_segmenter.hpp"

#include "unicode_grapheme_data.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace zevryon::text {
namespace {

void clear_error(GraphemeError* error) noexcept {
    if (error != nullptr) {
        error->kind = GraphemeErrorKind::None;
        error->codepoint_index = 0U;
        error->message.clear();
    }
}

bool fail(
    GraphemeErrorKind kind,
    std::size_t index,
    const char* message,
    GraphemeError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->codepoint_index = index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool is_control(GraphemeBreakClass value) noexcept {
    return value == GraphemeBreakClass::Control ||
           value == GraphemeBreakClass::CR ||
           value == GraphemeBreakClass::LF;
}

bool hangul_no_break(
    GraphemeBreakClass left,
    GraphemeBreakClass right) noexcept {
    if (left == GraphemeBreakClass::L) {
        return right == GraphemeBreakClass::L ||
               right == GraphemeBreakClass::V ||
               right == GraphemeBreakClass::LV ||
               right == GraphemeBreakClass::LVT;
    }
    if (left == GraphemeBreakClass::LV || left == GraphemeBreakClass::V) {
        return right == GraphemeBreakClass::V ||
               right == GraphemeBreakClass::T;
    }
    if (left == GraphemeBreakClass::LVT || left == GraphemeBreakClass::T) {
        return right == GraphemeBreakClass::T;
    }
    return false;
}

struct BoundaryState {
    std::uint64_t consecutive_regional_indicators{0};
    bool indic_chain_active{false};
    bool indic_chain_has_linker{false};
    bool extended_pictographic_extend_chain{false};
    bool left_zwj_has_extended_pictographic{false};
};

void consume_property(
    const GraphemeProperties& property,
    BoundaryState* state) noexcept {
    if (property.break_class == GraphemeBreakClass::RegionalIndicator) {
        ++state->consecutive_regional_indicators;
    } else {
        state->consecutive_regional_indicators = 0U;
    }

    if (property.indic_conjunct_break == IndicConjunctBreak::Consonant) {
        state->indic_chain_active = true;
        state->indic_chain_has_linker = false;
    } else if (state->indic_chain_active &&
               property.indic_conjunct_break == IndicConjunctBreak::Linker) {
        state->indic_chain_has_linker = true;
    } else if (!(state->indic_chain_active &&
                 property.indic_conjunct_break == IndicConjunctBreak::Extend)) {
        state->indic_chain_active = false;
        state->indic_chain_has_linker = false;
    }

    const bool prior_extended_chain =
        state->extended_pictographic_extend_chain;
    state->left_zwj_has_extended_pictographic =
        property.break_class == GraphemeBreakClass::ZWJ &&
        prior_extended_chain;
    if (property.extended_pictographic) {
        state->extended_pictographic_extend_chain = true;
    } else if (!(property.break_class == GraphemeBreakClass::Extend &&
                 prior_extended_chain)) {
        state->extended_pictographic_extend_chain = false;
    }
}

bool should_break(
    const GraphemeProperties& left,
    const GraphemeProperties& right,
    const BoundaryState& state) noexcept {
    // GB3
    if (left.break_class == GraphemeBreakClass::CR &&
        right.break_class == GraphemeBreakClass::LF) {
        return false;
    }
    // GB4 and GB5
    if (is_control(left.break_class) || is_control(right.break_class)) {
        return true;
    }
    // GB6 through GB8
    if (hangul_no_break(left.break_class, right.break_class)) {
        return false;
    }
    // GB9
    if (right.break_class == GraphemeBreakClass::Extend ||
        right.break_class == GraphemeBreakClass::ZWJ) {
        return false;
    }
    // GB9a
    if (right.break_class == GraphemeBreakClass::SpacingMark) {
        return false;
    }
    // GB9b
    if (left.break_class == GraphemeBreakClass::Prepend) {
        return false;
    }
    // GB9c
    if (right.indic_conjunct_break == IndicConjunctBreak::Consonant &&
        state.indic_chain_active && state.indic_chain_has_linker) {
        return false;
    }
    // GB11
    if (left.break_class == GraphemeBreakClass::ZWJ &&
        state.left_zwj_has_extended_pictographic &&
        right.extended_pictographic) {
        return false;
    }
    // GB12 and GB13
    if (left.break_class == GraphemeBreakClass::RegionalIndicator &&
        right.break_class == GraphemeBreakClass::RegionalIndicator &&
        (state.consecutive_regional_indicators % 2U) == 1U) {
        return false;
    }
    // GB999
    return true;
}

bool emit_cluster(
    std::span<const DecodedCodePoint> codepoints,
    std::size_t first,
    std::size_t end,
    std::pmr::vector<GraphemeCluster>* clusters,
    GraphemeSegmentStats* stats,
    GraphemeError* error) noexcept {
    if (first >= end || end > codepoints.size()) {
        return fail(
            GraphemeErrorKind::InvalidInput,
            first,
            "invalid grapheme cluster index range",
            error);
    }
    if (first > static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max()) ||
        end - first > static_cast<std::size_t>(
                          std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            GraphemeErrorKind::SourceRangeOverflow,
            first,
            "grapheme codepoint range exceeds 32-bit storage",
            error);
    }

    const std::uint64_t source_start = codepoints[first].source_start;
    const std::uint64_t source_end = codepoints[end - 1U].source_end();
    if (source_end < source_start ||
        source_end - source_start >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            GraphemeErrorKind::SourceRangeOverflow,
            first,
            "grapheme source range exceeds 32-bit storage",
            error);
    }

    const std::uint32_t codepoint_count =
        static_cast<std::uint32_t>(end - first);
    const std::uint32_t source_length =
        static_cast<std::uint32_t>(source_end - source_start);
    try {
        clusters->push_back({
            source_start,
            source_length,
            static_cast<std::uint32_t>(first),
            codepoint_count,
        });
    } catch (const std::bad_alloc&) {
        return fail(
            GraphemeErrorKind::OutputBudgetExceeded,
            first,
            "grapheme output exceeded its resource budget",
            error);
    } catch (...) {
        return fail(
            GraphemeErrorKind::OutputBudgetExceeded,
            first,
            "grapheme output allocation failed",
            error);
    }

    ++stats->output_clusters;
    stats->maximum_cluster_codepoints = std::max(
        stats->maximum_cluster_codepoints,
        static_cast<std::uint64_t>(codepoint_count));
    stats->maximum_cluster_source_bytes = std::max(
        stats->maximum_cluster_source_bytes,
        static_cast<std::uint64_t>(source_length));
    return true;
}

} // namespace

const char* grapheme_error_kind_name(GraphemeErrorKind kind) noexcept {
    switch (kind) {
        case GraphemeErrorKind::None:
            return "none";
        case GraphemeErrorKind::InvalidInput:
            return "invalid_input";
        case GraphemeErrorKind::SourceRangeOverflow:
            return "source_range_overflow";
        case GraphemeErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
    }
    return "invalid";
}

bool segment_graphemes(
    std::span<const DecodedCodePoint> codepoints,
    std::pmr::vector<GraphemeCluster>* clusters,
    GraphemeSegmentStats* stats,
    GraphemeError* error) noexcept {
    if (clusters == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    clear_error(error);
    clusters->clear();
    *stats = {};
    stats->input_codepoints = static_cast<std::uint64_t>(codepoints.size());
    if (codepoints.empty()) {
        return true;
    }
    if (codepoints.size() > static_cast<std::size_t>(
                                std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            GraphemeErrorKind::SourceRangeOverflow,
            0U,
            "grapheme input exceeds 32-bit codepoint indexing",
            error);
    }

    for (std::size_t index = 0U; index < codepoints.size(); ++index) {
        const DecodedCodePoint& codepoint = codepoints[index];
        if (codepoint.source_length == 0U || codepoint.source_length > 4U) {
            return fail(
                GraphemeErrorKind::InvalidInput,
                index,
                "decoded codepoint has an invalid UTF-8 source length",
                error);
        }
        if (codepoint.value > 0x10ffffU ||
            (codepoint.value >= 0xd800U && codepoint.value <= 0xdfffU)) {
            return fail(
                GraphemeErrorKind::InvalidInput,
                index,
                "decoded codepoint is not a Unicode scalar value",
                error);
        }
        if (index != 0U &&
            codepoint.source_start != codepoints[index - 1U].source_end()) {
            return fail(
                GraphemeErrorKind::InvalidInput,
                index,
                "decoded codepoint source ranges are not contiguous",
                error);
        }
    }

    std::size_t cluster_start = 0U;
    GraphemeProperties left = grapheme_properties(codepoints[0].value);
    BoundaryState state;
    consume_property(left, &state);

    for (std::size_t index = 1U; index < codepoints.size(); ++index) {
        const GraphemeProperties right =
            grapheme_properties(codepoints[index].value);
        if (should_break(left, right, state)) {
            if (!emit_cluster(
                    codepoints,
                    cluster_start,
                    index,
                    clusters,
                    stats,
                    error)) {
                return false;
            }
            cluster_start = index;
        } else {
            ++stats->suppressed_breaks;
        }
        consume_property(right, &state);
        left = right;
    }

    return emit_cluster(
        codepoints,
        cluster_start,
        codepoints.size(),
        clusters,
        stats,
        error);
}

} // namespace zevryon::text
