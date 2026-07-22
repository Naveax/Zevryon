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

bool emit_boundary(
    std::uint64_t source_offset,
    std::size_t codepoint_index,
    std::pmr::vector<GraphemeBoundary>* boundaries,
    GraphemeError* error) noexcept {
    if (codepoint_index > static_cast<std::size_t>(
                              std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            GraphemeErrorKind::SourceRangeOverflow,
            codepoint_index,
            "grapheme boundary exceeds 32-bit codepoint indexing",
            error);
    }
    try {
        boundaries->push_back({
            source_offset,
            static_cast<std::uint32_t>(codepoint_index),
        });
    } catch (const std::bad_alloc&) {
        return fail(
            GraphemeErrorKind::OutputBudgetExceeded,
            codepoint_index,
            "grapheme boundary output exceeded its resource budget",
            error);
    } catch (...) {
        return fail(
            GraphemeErrorKind::OutputBudgetExceeded,
            codepoint_index,
            "grapheme boundary output allocation failed",
            error);
    }
    return true;
}

bool record_cluster_stats(
    std::span<const DecodedCodePoint> codepoints,
    std::size_t first,
    std::size_t end,
    GraphemeSegmentStats* stats,
    GraphemeError* error) noexcept {
    if (first >= end || end > codepoints.size()) {
        return fail(
            GraphemeErrorKind::InvalidInput,
            first,
            "invalid grapheme cluster index range",
            error);
    }
    const std::uint64_t source_start = codepoints[first].source_start;
    const std::uint64_t source_end = codepoints[end - 1U].source_end();
    if (source_end < source_start) {
        return fail(
            GraphemeErrorKind::SourceRangeOverflow,
            first,
            "grapheme source range overflowed",
            error);
    }
    stats->maximum_cluster_codepoints = std::max(
        stats->maximum_cluster_codepoints,
        static_cast<std::uint64_t>(end - first));
    stats->maximum_cluster_source_bytes = std::max(
        stats->maximum_cluster_source_bytes,
        source_end - source_start);
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
    std::pmr::vector<GraphemeBoundary>* boundaries,
    GraphemeSegmentStats* stats,
    GraphemeError* error) noexcept {
    if (boundaries == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    clear_error(error);
    boundaries->clear();
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
        if (codepoint.source_end() < codepoint.source_start) {
            return fail(
                GraphemeErrorKind::SourceRangeOverflow,
                index,
                "decoded codepoint source range overflowed",
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

    if (!emit_boundary(
            codepoints.front().source_start,
            0U,
            boundaries,
            error)) {
        return false;
    }

    std::size_t cluster_start = 0U;
    GraphemeProperties left = grapheme_properties(codepoints[0].value);
    BoundaryState state;
    consume_property(left, &state);

    for (std::size_t index = 1U; index < codepoints.size(); ++index) {
        const GraphemeProperties right =
            grapheme_properties(codepoints[index].value);
        if (should_break(left, right, state)) {
            if (!record_cluster_stats(
                    codepoints,
                    cluster_start,
                    index,
                    stats,
                    error) ||
                !emit_boundary(
                    codepoints[index].source_start,
                    index,
                    boundaries,
                    error)) {
                return false;
            }
            ++stats->output_clusters;
            cluster_start = index;
        } else {
            ++stats->suppressed_breaks;
        }
        consume_property(right, &state);
        left = right;
    }

    if (!record_cluster_stats(
            codepoints,
            cluster_start,
            codepoints.size(),
            stats,
            error) ||
        !emit_boundary(
            codepoints.back().source_end(),
            codepoints.size(),
            boundaries,
            error)) {
        return false;
    }
    ++stats->output_clusters;
    return true;
}

} // namespace zevryon::text
