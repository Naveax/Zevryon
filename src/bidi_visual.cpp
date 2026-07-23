#include "bidi_visual.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace zevryon::text {
namespace {

constexpr std::uint8_t kMaximumResolvedLevel = 126U;

void clear_error(BidiVisualError* error) noexcept {
    if (error != nullptr) {
        error->kind = BidiVisualErrorKind::None;
        error->active_index = 0U;
        error->message.clear();
    }
}

bool fail(
    BidiVisualErrorKind kind,
    std::size_t active_index,
    const char* message,
    BidiVisualError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->active_index = active_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

template <typename T>
void release_vector(std::pmr::vector<T>* values) noexcept {
    std::pmr::vector<T> empty(values->get_allocator().resource());
    values->swap(empty);
}

bool valid_bidi_class(BidiClass value) noexcept {
    return static_cast<std::size_t>(value) <
           static_cast<std::size_t>(BidiClass::Count);
}

bool is_l1_separator(BidiClass value) noexcept {
    return value == BidiClass::B || value == BidiClass::S;
}

bool is_l1_whitespace(BidiClass value) noexcept {
    return value == BidiClass::WS;
}

bool is_l1_isolate_format(BidiClass value) noexcept {
    return value == BidiClass::FSI ||
           value == BidiClass::LRI ||
           value == BidiClass::RLI ||
           value == BidiClass::PDI;
}

bool is_l1_trailing_type(BidiClass value) noexcept {
    return is_l1_whitespace(value) || is_l1_isolate_format(value);
}

bool validate_inputs(
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    std::span<const std::uint8_t> implicit_levels,
    std::uint8_t paragraph_level,
    std::span<const BidiLineSpan> lines,
    BidiVisualError* error) noexcept {
    const std::size_t active_count = topology.active_unit_indices.size();
    if (paragraph_level > 1U ||
        implicit_levels.size() != active_count ||
        active_count > static_cast<std::size_t>(
                           std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            BidiVisualErrorKind::InvalidInput,
            0U,
            "paragraph level or active visual input size is invalid",
            error);
    }

    for (std::size_t active = 0U; active < active_count; ++active) {
        const std::uint32_t unit_index = topology.active_unit_indices[active];
        if (unit_index >= units.size() ||
            (active != 0U &&
             topology.active_unit_indices[active - 1U] >= unit_index) ||
            (units[unit_index].flags & kBidiUnitRemovedByX9) != 0U ||
            !valid_bidi_class(units[unit_index].original_class) ||
            implicit_levels[active] > kMaximumResolvedLevel ||
            implicit_levels[active] < units[unit_index].level) {
            return fail(
                BidiVisualErrorKind::InvalidInput,
                active,
                "active indices, original types, or implicit levels are invalid",
                error);
        }
    }

    if (active_count == 0U) {
        if (!lines.empty()) {
            return fail(
                BidiVisualErrorKind::LinePartitionViolation,
                0U,
                "empty active stream must have no visual lines",
                error);
        }
        return true;
    }
    if (lines.empty()) {
        return fail(
            BidiVisualErrorKind::LinePartitionViolation,
            0U,
            "non-empty active stream requires at least one line",
            error);
    }

    std::size_t expected_first = 0U;
    for (const BidiLineSpan& line : lines) {
        if (line.active_count == 0U ||
            line.first_active_index != expected_first ||
            static_cast<std::size_t>(line.first_active_index) +
                    static_cast<std::size_t>(line.active_count) >
                active_count) {
            return fail(
                BidiVisualErrorKind::LinePartitionViolation,
                expected_first,
                "line spans do not form a non-empty contiguous partition",
                error);
        }
        expected_first += line.active_count;
    }
    if (expected_first != active_count) {
        return fail(
            BidiVisualErrorKind::LinePartitionViolation,
            expected_first,
            "line spans do not cover every active scalar",
            error);
    }
    return true;
}

void record_l1_reset(
    BidiClass original,
    BidiVisualStats* stats) noexcept {
    if (is_l1_separator(original)) {
        ++stats->l1_separator_resets;
    } else if (is_l1_isolate_format(original)) {
        ++stats->l1_isolate_resets;
    } else {
        ++stats->l1_whitespace_resets;
    }
}

void reset_level_if_needed(
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    std::uint32_t active,
    std::uint8_t paragraph_level,
    std::pmr::vector<std::uint8_t>* levels,
    BidiVisualStats* stats) noexcept {
    if ((*levels)[active] == paragraph_level) {
        return;
    }
    (*levels)[active] = paragraph_level;
    record_l1_reset(
        units[topology.active_unit_indices[active]].original_class,
        stats);
}

void apply_l1(
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    std::uint8_t paragraph_level,
    const BidiLineSpan& line,
    std::pmr::vector<std::uint8_t>* levels,
    BidiVisualStats* stats) noexcept {
    const std::uint32_t first = line.first_active_index;
    const std::uint32_t end = static_cast<std::uint32_t>(
        static_cast<std::size_t>(first) + line.active_count);

    for (std::uint32_t active = first; active < end; ++active) {
        const BidiClass original =
            units[topology.active_unit_indices[active]].original_class;
        if (!is_l1_separator(original)) {
            continue;
        }
        reset_level_if_needed(
            units,
            topology,
            active,
            paragraph_level,
            levels,
            stats);
        std::uint32_t preceding = active;
        while (preceding > first) {
            const std::uint32_t candidate = preceding - 1U;
            const BidiClass candidate_original =
                units[topology.active_unit_indices[candidate]].original_class;
            if (!is_l1_trailing_type(candidate_original)) {
                break;
            }
            reset_level_if_needed(
                units,
                topology,
                candidate,
                paragraph_level,
                levels,
                stats);
            preceding = candidate;
        }
    }

    std::uint32_t trailing = end;
    while (trailing > first) {
        const std::uint32_t candidate = trailing - 1U;
        const BidiClass original =
            units[topology.active_unit_indices[candidate]].original_class;
        if (!is_l1_trailing_type(original)) {
            break;
        }
        reset_level_if_needed(
            units,
            topology,
            candidate,
            paragraph_level,
            levels,
            stats);
        trailing = candidate;
    }
}

void apply_l2(
    const BidiLineSpan& line,
    std::span<const std::uint8_t> levels,
    std::pmr::vector<std::uint32_t>* order,
    BidiVisualStats* stats) noexcept {
    const std::size_t first = line.first_active_index;
    const std::size_t end = first + line.active_count;
    std::uint8_t maximum = 0U;
    std::uint8_t lowest_odd = kMaximumResolvedLevel;
    for (std::size_t position = first; position < end; ++position) {
        const std::uint8_t level = levels[position];
        maximum = std::max(maximum, level);
        if ((level & 1U) != 0U) {
            lowest_odd = std::min(lowest_odd, level);
        }
    }
    stats->maximum_line_level =
        std::max(stats->maximum_line_level, maximum);
    if (lowest_odd == kMaximumResolvedLevel) {
        return;
    }

    for (std::uint16_t threshold = maximum;
         threshold >= lowest_odd;
         --threshold) {
        std::size_t position = first;
        while (position < end) {
            while (position < end &&
                   levels[(*order)[position]] < threshold) {
                ++position;
            }
            const std::size_t span_first = position;
            while (position < end &&
                   levels[(*order)[position]] >= threshold) {
                ++position;
            }
            if (position - span_first > 1U) {
                std::reverse(
                    order->begin() + static_cast<std::ptrdiff_t>(span_first),
                    order->begin() + static_cast<std::ptrdiff_t>(position));
                ++stats->l2_reversal_spans;
                stats->l2_reversed_units += position - span_first;
            }
        }
        if (threshold == lowest_odd) {
            break;
        }
    }
}

void apply_l3(
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    const BidiLineSpan& line,
    std::pmr::vector<std::uint32_t>* order,
    BidiVisualStats* stats) noexcept {
    const std::size_t first = line.first_active_index;
    const std::size_t end = first + line.active_count;
    std::size_t position = first;
    while (position < end) {
        const std::uint32_t first_active = (*order)[position];
        const BidiClass first_original =
            units[topology.active_unit_indices[first_active]].original_class;
        if (first_original != BidiClass::NSM) {
            ++position;
            continue;
        }

        std::size_t after_marks = position + 1U;
        std::uint32_t previous_active = first_active;
        while (after_marks < end) {
            const std::uint32_t active = (*order)[after_marks];
            const BidiClass original =
                units[topology.active_unit_indices[active]].original_class;
            if (original != BidiClass::NSM ||
                active + 1U != previous_active) {
                break;
            }
            previous_active = active;
            ++after_marks;
        }
        if (after_marks >= end) {
            break;
        }

        const std::uint32_t base_active = (*order)[after_marks];
        const BidiClass base_original =
            units[topology.active_unit_indices[base_active]].original_class;
        const std::size_t mark_count = after_marks - position;
        if (base_original == BidiClass::NSM ||
            static_cast<std::size_t>(base_active) + mark_count != first_active) {
            ++position;
            continue;
        }

        std::reverse(
            order->begin() + static_cast<std::ptrdiff_t>(position),
            order->begin() + static_cast<std::ptrdiff_t>(after_marks + 1U));
        ++stats->l3_combining_sequences;
        stats->l3_repaired_units += mark_count + 1U;
        position = after_marks + 1U;
    }
}

} // namespace

BidiVisualOrder::BidiVisualOrder(std::pmr::memory_resource* resource)
    : line_levels(resource), visual_to_active(resource) {}

std::pmr::memory_resource* BidiVisualOrder::resource() const noexcept {
    return line_levels.get_allocator().resource();
}

void BidiVisualOrder::release() noexcept {
    release_vector(&line_levels);
    release_vector(&visual_to_active);
}

const char* bidi_visual_error_kind_name(BidiVisualErrorKind kind) noexcept {
    switch (kind) {
        case BidiVisualErrorKind::None:
            return "none";
        case BidiVisualErrorKind::InvalidInput:
            return "invalid_input";
        case BidiVisualErrorKind::TopologyViolation:
            return "topology_violation";
        case BidiVisualErrorKind::LinePartitionViolation:
            return "line_partition_violation";
        case BidiVisualErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
    }
    return "invalid";
}

bool resolve_bidi_visual_order(
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    std::span<const std::uint8_t> implicit_levels,
    std::uint8_t paragraph_level,
    std::span<const BidiLineSpan> lines,
    BidiVisualOrder* output,
    BidiVisualStats* stats,
    BidiVisualError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    clear_error(error);
    output->release();
    *stats = {};
    stats->input_units = static_cast<std::uint64_t>(units.size());

    try {
        if (!validate_inputs(
                units,
                topology,
                implicit_levels,
                paragraph_level,
                lines,
                error)) {
            return false;
        }

        BidiVisualOrder working(output->resource());
        working.line_levels.assign(
            implicit_levels.begin(),
            implicit_levels.end());
        working.visual_to_active.resize(implicit_levels.size());
        for (std::size_t active = 0U; active < implicit_levels.size(); ++active) {
            working.visual_to_active[active] =
                static_cast<std::uint32_t>(active);
            stats->maximum_input_level =
                std::max(stats->maximum_input_level, implicit_levels[active]);
        }

        stats->active_units =
            static_cast<std::uint64_t>(implicit_levels.size());
        stats->lines = static_cast<std::uint64_t>(lines.size());
        for (const BidiLineSpan& line : lines) {
            apply_l1(
                units,
                topology,
                paragraph_level,
                line,
                &working.line_levels,
                stats);
            apply_l2(
                line,
                working.line_levels,
                &working.visual_to_active,
                stats);
            apply_l3(
                units,
                topology,
                line,
                &working.visual_to_active,
                stats);
        }

        stats->output_levels =
            static_cast<std::uint64_t>(working.line_levels.size());
        stats->output_visual_indices =
            static_cast<std::uint64_t>(working.visual_to_active.size());
        output->line_levels.swap(working.line_levels);
        output->visual_to_active.swap(working.visual_to_active);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(
            BidiVisualErrorKind::OutputBudgetExceeded,
            0U,
            "visual-order resolution exceeded its resource budget",
            error);
    } catch (...) {
        return fail(
            BidiVisualErrorKind::OutputBudgetExceeded,
            0U,
            "visual-order resolution allocation failed",
            error);
    }
}

} // namespace zevryon::text
