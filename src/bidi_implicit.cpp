#include "bidi_implicit.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace zevryon::text {
namespace {

constexpr std::uint8_t kMaximumExplicitLevel = 125U;
constexpr std::uint8_t kMaximumImplicitLevel = 126U;

void clear_error(BidiImplicitError* error) noexcept {
    if (error != nullptr) {
        error->kind = BidiImplicitErrorKind::None;
        error->active_index = 0U;
        error->message.clear();
    }
}

bool fail(
    BidiImplicitErrorKind kind,
    std::size_t active_index,
    const char* message,
    BidiImplicitError* error) noexcept {
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

bool final_implicit_type(BidiClass value) noexcept {
    return value == BidiClass::L ||
           value == BidiClass::R ||
           value == BidiClass::EN ||
           value == BidiClass::AN;
}

bool mark_run_once(
    std::pmr::vector<std::uint64_t>* visited,
    std::uint32_t run_id) noexcept {
    const std::size_t word = static_cast<std::size_t>(run_id) / 64U;
    const std::uint32_t bit = run_id % 64U;
    const std::uint64_t mask = std::uint64_t{1U} << bit;
    if (((*visited)[word] & mask) != 0U) {
        return false;
    }
    (*visited)[word] |= mask;
    return true;
}

bool validate_topology(
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    std::span<const BidiClass> neutral_types,
    std::pmr::memory_resource* resource,
    BidiImplicitError* error) {
    const std::size_t active_count = topology.active_unit_indices.size();
    if (neutral_types.size() != active_count ||
        active_count > static_cast<std::size_t>(
                           std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            BidiImplicitErrorKind::InvalidInput,
            0U,
            "neutral types do not match the 32-bit active-unit stream",
            error);
    }

    for (std::size_t active = 0U; active < active_count; ++active) {
        const std::uint32_t unit_index = topology.active_unit_indices[active];
        if (unit_index >= units.size() ||
            (active != 0U &&
             topology.active_unit_indices[active - 1U] >= unit_index) ||
            (units[unit_index].flags & kBidiUnitRemovedByX9) != 0U ||
            units[unit_index].level > kMaximumExplicitLevel ||
            !final_implicit_type(neutral_types[active])) {
            return fail(
                BidiImplicitErrorKind::InvalidInput,
                active,
                "active indices, explicit levels, or N0-N2 types are invalid",
                error);
        }
    }

    std::size_t expected_active = 0U;
    for (std::size_t run_index = 0U;
         run_index < topology.level_runs.size();
         ++run_index) {
        const BidiLevelRun& run = topology.level_runs[run_index];
        if (run.active_count == 0U ||
            run.level > kMaximumExplicitLevel ||
            run.first_active_index != expected_active ||
            static_cast<std::size_t>(run.first_active_index) +
                    static_cast<std::size_t>(run.active_count) >
                active_count) {
            return fail(
                BidiImplicitErrorKind::TopologyViolation,
                expected_active,
                "level runs do not form a bounded contiguous active partition",
                error);
        }
        const std::size_t end =
            static_cast<std::size_t>(run.first_active_index) + run.active_count;
        for (std::size_t active = run.first_active_index;
             active < end;
             ++active) {
            if (units[topology.active_unit_indices[active]].level != run.level) {
                return fail(
                    BidiImplicitErrorKind::TopologyViolation,
                    active,
                    "level-run level disagrees with explicit bidi units",
                    error);
            }
        }
        expected_active = end;
    }
    if (expected_active != active_count ||
        topology.sequence_run_indices.size() != topology.level_runs.size()) {
        return fail(
            BidiImplicitErrorKind::TopologyViolation,
            expected_active,
            "level runs or sequence links do not cover the active stream",
            error);
    }

    std::pmr::vector<std::uint64_t> visited(resource);
    try {
        visited.assign(
            (topology.level_runs.size() + 63U) / 64U,
            std::uint64_t{0U});
    } catch (const std::bad_alloc&) {
        return fail(
            BidiImplicitErrorKind::OutputBudgetExceeded,
            0U,
            "implicit topology validation exceeded its resource budget",
            error);
    }

    std::size_t expected_link = 0U;
    for (const BidiIsolatingRunSequence& sequence : topology.sequences) {
        if (sequence.run_count == 0U ||
            sequence.level > kMaximumExplicitLevel ||
            sequence.first_run_link != expected_link ||
            (sequence.sos != BidiClass::L && sequence.sos != BidiClass::R) ||
            (sequence.eos != BidiClass::L && sequence.eos != BidiClass::R) ||
            static_cast<std::size_t>(sequence.first_run_link) +
                    static_cast<std::size_t>(sequence.run_count) >
                topology.sequence_run_indices.size()) {
            return fail(
                BidiImplicitErrorKind::TopologyViolation,
                0U,
                "isolating sequence descriptors are invalid",
                error);
        }

        std::uint32_t previous_active = 0U;
        bool have_previous = false;
        for (std::size_t offset = 0U; offset < sequence.run_count; ++offset) {
            const std::size_t link =
                static_cast<std::size_t>(sequence.first_run_link) + offset;
            const std::uint32_t run_id = topology.sequence_run_indices[link];
            if (run_id >= topology.level_runs.size() ||
                !mark_run_once(&visited, run_id) ||
                topology.level_runs[run_id].level != sequence.level) {
                return fail(
                    BidiImplicitErrorKind::TopologyViolation,
                    0U,
                    "isolating sequences duplicate or mismatch level runs",
                    error);
            }
            const BidiLevelRun& run = topology.level_runs[run_id];
            if (have_previous && run.first_active_index <= previous_active) {
                return fail(
                    BidiImplicitErrorKind::TopologyViolation,
                    run.first_active_index,
                    "sequence run links are not in logical order",
                    error);
            }
            previous_active = static_cast<std::uint32_t>(
                static_cast<std::size_t>(run.first_active_index) +
                static_cast<std::size_t>(run.active_count) - 1U);
            have_previous = true;
        }
        expected_link += sequence.run_count;
    }
    if (expected_link != topology.sequence_run_indices.size()) {
        return fail(
            BidiImplicitErrorKind::TopologyViolation,
            0U,
            "isolating sequences do not cover every run link",
            error);
    }

    const std::size_t run_count = topology.level_runs.size();
    for (std::size_t run = 0U; run < run_count; ++run) {
        const std::size_t word = run / 64U;
        const std::uint32_t bit = static_cast<std::uint32_t>(run % 64U);
        if ((visited[word] & (std::uint64_t{1U} << bit)) == 0U) {
            return fail(
                BidiImplicitErrorKind::TopologyViolation,
                0U,
                "a level run is missing from isolating sequences",
                error);
        }
    }
    return true;
}

} // namespace

const char* bidi_implicit_error_kind_name(BidiImplicitErrorKind kind) noexcept {
    switch (kind) {
        case BidiImplicitErrorKind::None:
            return "none";
        case BidiImplicitErrorKind::InvalidInput:
            return "invalid_input";
        case BidiImplicitErrorKind::TopologyViolation:
            return "topology_violation";
        case BidiImplicitErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
    }
    return "invalid";
}

bool resolve_bidi_implicit_levels(
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    std::span<const BidiClass> neutral_types,
    std::pmr::vector<std::uint8_t>* resolved_levels,
    BidiImplicitStats* stats,
    BidiImplicitError* error) noexcept {
    if (resolved_levels == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    clear_error(error);
    release_vector(resolved_levels);
    *stats = {};
    stats->input_units = static_cast<std::uint64_t>(units.size());

    try {
        if (!validate_topology(
                units,
                topology,
                neutral_types,
                resolved_levels->get_allocator().resource(),
                error)) {
            return false;
        }

        std::pmr::vector<std::uint8_t> working(
            resolved_levels->get_allocator().resource());
        working.resize(neutral_types.size());
        stats->active_units = static_cast<std::uint64_t>(neutral_types.size());
        stats->isolating_sequences =
            static_cast<std::uint64_t>(topology.sequences.size());

        for (std::size_t active = 0U; active < neutral_types.size(); ++active) {
            const BidiExplicitUnit& unit =
                units[topology.active_unit_indices[active]];
            const std::uint8_t input_level = unit.level;
            std::uint8_t output_level = input_level;
            const BidiClass type = neutral_types[active];

            if ((input_level & 1U) == 0U) {
                if (type == BidiClass::R) {
                    output_level = static_cast<std::uint8_t>(input_level + 1U);
                    ++stats->i1_r_changes;
                } else if (type == BidiClass::EN || type == BidiClass::AN) {
                    output_level = static_cast<std::uint8_t>(input_level + 2U);
                    ++stats->i1_number_changes;
                }
            } else if (type == BidiClass::L) {
                output_level = static_cast<std::uint8_t>(input_level + 1U);
                ++stats->i2_l_changes;
            } else if (type == BidiClass::EN || type == BidiClass::AN) {
                output_level = static_cast<std::uint8_t>(input_level + 1U);
                ++stats->i2_number_changes;
            }

            if (output_level > kMaximumImplicitLevel) {
                return fail(
                    BidiImplicitErrorKind::InvalidInput,
                    active,
                    "I1-I2 produced a level above max_depth+1",
                    error);
            }
            working[active] = output_level;
            stats->maximum_input_level =
                std::max(stats->maximum_input_level, input_level);
            stats->maximum_output_level =
                std::max(stats->maximum_output_level, output_level);
        }

        stats->output_levels = static_cast<std::uint64_t>(working.size());
        resolved_levels->swap(working);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(
            BidiImplicitErrorKind::OutputBudgetExceeded,
            0U,
            "implicit-level resolution exceeded its resource budget",
            error);
    } catch (...) {
        return fail(
            BidiImplicitErrorKind::OutputBudgetExceeded,
            0U,
            "implicit-level resolution allocation failed",
            error);
    }
}

} // namespace zevryon::text
