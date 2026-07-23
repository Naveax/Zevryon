#include "bidi_sequence.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <new>

namespace zevryon::text {
namespace {

constexpr std::uint32_t kNoIndex = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint8_t kAllowedFlags =
    kBidiUnitRemovedByX9 |
    kBidiUnitIsolateInitiator |
    kBidiUnitPopDirectionalIsolate;

void clear_error(BidiSequenceError* error) noexcept {
    if (error != nullptr) {
        error->kind = BidiSequenceErrorKind::None;
        error->unit_index = 0U;
        error->message.clear();
    }
}

bool fail(
    BidiSequenceErrorKind kind,
    std::size_t unit_index,
    const char* message,
    BidiSequenceError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->unit_index = unit_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool is_x9_class(BidiClass value) noexcept {
    return value == BidiClass::RLE ||
           value == BidiClass::LRE ||
           value == BidiClass::RLO ||
           value == BidiClass::LRO ||
           value == BidiClass::PDF ||
           value == BidiClass::BN;
}

bool valid_bidi_class(BidiClass value) noexcept {
    return static_cast<std::size_t>(value) <
           static_cast<std::size_t>(BidiClass::Count);
}

BidiClass direction_for_level(std::uint8_t level) noexcept {
    return (level & 1U) == 0U ? BidiClass::L : BidiClass::R;
}

bool validate_input(
    std::span<const BidiExplicitUnit> units,
    std::uint8_t paragraph_level,
    BidiSequenceError* error) noexcept {
    if (paragraph_level > 1U) {
        return fail(
            BidiSequenceErrorKind::InvalidInput,
            0U,
            "paragraph level must be zero or one",
            error);
    }
    if (units.size() > static_cast<std::size_t>(kNoIndex)) {
        return fail(
            BidiSequenceErrorKind::IndexOverflow,
            0U,
            "bidi unit count exceeds 32-bit topology indices",
            error);
    }

    for (std::size_t index = 0U; index < units.size(); ++index) {
        const BidiExplicitUnit& unit = units[index];
        const bool expected_removed = is_x9_class(unit.original_class);
        const bool actual_removed =
            (unit.flags & kBidiUnitRemovedByX9) != 0U;
        const bool expected_initiator =
            is_bidi_isolate_initiator(unit.original_class);
        const bool actual_initiator =
            (unit.flags & kBidiUnitIsolateInitiator) != 0U;
        const bool expected_pdi = unit.original_class == BidiClass::PDI;
        const bool actual_pdi =
            (unit.flags & kBidiUnitPopDirectionalIsolate) != 0U;

        if (unit.codepoint_index != index ||
            unit.level > 125U ||
            !valid_bidi_class(unit.original_class) ||
            !valid_bidi_class(unit.resolved_class) ||
            (unit.flags & static_cast<std::uint8_t>(~kAllowedFlags)) != 0U ||
            expected_removed != actual_removed ||
            expected_initiator != actual_initiator ||
            expected_pdi != actual_pdi ||
            (index != 0U &&
             unit.source_offset <= units[index - 1U].source_offset)) {
            return fail(
                BidiSequenceErrorKind::InvalidInput,
                index,
                "explicit bidi units violate the X9/X10 input contract",
                error);
        }
        if (unit.original_class == BidiClass::B && index + 1U != units.size()) {
            return fail(
                BidiSequenceErrorKind::InvalidInput,
                index,
                "paragraph separator must be the final explicit unit",
                error);
        }
    }
    return true;
}

template <typename T>
void release_vector(std::pmr::vector<T>* values) noexcept {
    std::pmr::vector<T> empty(values->get_allocator().resource());
    values->swap(empty);
}

} // namespace

const char* bidi_sequence_error_kind_name(BidiSequenceErrorKind kind) noexcept {
    switch (kind) {
        case BidiSequenceErrorKind::None:
            return "none";
        case BidiSequenceErrorKind::InvalidInput:
            return "invalid_input";
        case BidiSequenceErrorKind::IndexOverflow:
            return "index_overflow";
        case BidiSequenceErrorKind::TopologyViolation:
            return "topology_violation";
        case BidiSequenceErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
    }
    return "invalid";
}

BidiSequenceTopology::BidiSequenceTopology(
    std::pmr::memory_resource* resource)
    : active_unit_indices(
          resource == nullptr ? std::pmr::get_default_resource() : resource),
      level_runs(
          resource == nullptr ? std::pmr::get_default_resource() : resource),
      sequence_run_indices(
          resource == nullptr ? std::pmr::get_default_resource() : resource),
      sequences(
          resource == nullptr ? std::pmr::get_default_resource() : resource) {}

std::pmr::memory_resource* BidiSequenceTopology::resource() const noexcept {
    return active_unit_indices.get_allocator().resource();
}

void BidiSequenceTopology::release() noexcept {
    release_vector(&active_unit_indices);
    release_vector(&level_runs);
    release_vector(&sequence_run_indices);
    release_vector(&sequences);
}

bool build_bidi_isolating_run_sequences(
    std::span<const BidiExplicitUnit> units,
    std::uint8_t paragraph_level,
    BidiSequenceTopology* topology,
    BidiSequenceStats* stats,
    BidiSequenceError* error) noexcept {
    if (topology == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    clear_error(error);
    topology->release();
    *stats = {};
    stats->input_units = static_cast<std::uint64_t>(units.size());
    if (!validate_input(units, paragraph_level, error)) {
        return false;
    }
    if (units.empty()) {
        return true;
    }

    BidiSequenceTopology working(topology->resource());
    try {
        working.active_unit_indices.reserve(units.size());
        for (std::size_t index = 0U; index < units.size(); ++index) {
            if ((units[index].flags & kBidiUnitRemovedByX9) == 0U) {
                working.active_unit_indices.push_back(
                    static_cast<std::uint32_t>(index));
            }
        }
        stats->active_units = static_cast<std::uint64_t>(
            working.active_unit_indices.size());
        stats->removed_units = stats->input_units - stats->active_units;
        if (working.active_unit_indices.empty()) {
            working.active_unit_indices.swap(topology->active_unit_indices);
            return true;
        }

        const std::size_t active_count = working.active_unit_indices.size();
        std::pmr::vector<std::uint32_t> matching(topology->resource());
        std::pmr::vector<std::uint32_t> isolate_stack(topology->resource());
        matching.assign(active_count, kNoIndex);

        for (std::size_t active = 0U; active < active_count; ++active) {
            const std::uint32_t unit_index = working.active_unit_indices[active];
            const BidiClass original = units[unit_index].original_class;
            if (is_bidi_isolate_initiator(original)) {
                isolate_stack.push_back(static_cast<std::uint32_t>(active));
            } else if (original == BidiClass::PDI) {
                if (isolate_stack.empty()) {
                    ++stats->unmatched_pdi;
                } else {
                    const std::uint32_t initiator = isolate_stack.back();
                    isolate_stack.pop_back();
                    matching[initiator] = static_cast<std::uint32_t>(active);
                    matching[active] = initiator;
                    ++stats->matched_isolates;
                }
            }
        }
        stats->unmatched_isolate_initiators =
            static_cast<std::uint64_t>(isolate_stack.size());

        working.level_runs.reserve(active_count);
        std::size_t run_start = 0U;
        while (run_start < active_count) {
            const std::uint8_t level =
                units[working.active_unit_indices[run_start]].level;
            std::size_t run_end = run_start + 1U;
            while (run_end < active_count &&
                   units[working.active_unit_indices[run_end]].level == level) {
                ++run_end;
            }
            const std::size_t run_count = run_end - run_start;
            if (run_start > static_cast<std::size_t>(kNoIndex) ||
                run_count > static_cast<std::size_t>(kNoIndex)) {
                return fail(
                    BidiSequenceErrorKind::IndexOverflow,
                    working.active_unit_indices[run_start],
                    "level-run range exceeds 32-bit storage",
                    error);
            }
            working.level_runs.push_back({
                static_cast<std::uint32_t>(run_start),
                static_cast<std::uint32_t>(run_count),
                level,
                0U,
                0U,
            });
            run_start = run_end;
        }
        stats->level_runs = static_cast<std::uint64_t>(working.level_runs.size());

        const std::size_t level_run_count = working.level_runs.size();
        std::pmr::vector<std::uint32_t> next_run(topology->resource());
        std::pmr::vector<std::uint8_t> incoming(topology->resource());
        std::pmr::vector<std::uint8_t> visited(topology->resource());
        next_run.assign(level_run_count, kNoIndex);
        incoming.assign(level_run_count, std::uint8_t{0U});
        visited.assign(level_run_count, std::uint8_t{0U});

        for (std::size_t run_id = 0U; run_id < level_run_count; ++run_id) {
            const BidiLevelRun& run = working.level_runs[run_id];
            const std::size_t last_active =
                static_cast<std::size_t>(run.first_active_index) +
                static_cast<std::size_t>(run.active_count) - 1U;
            const std::uint32_t last_unit_index =
                working.active_unit_indices[last_active];
            if (!is_bidi_isolate_initiator(
                    units[last_unit_index].original_class) ||
                matching[last_active] == kNoIndex) {
                continue;
            }

            const std::uint32_t target_active = matching[last_active];
            const auto target_iterator = std::lower_bound(
                working.level_runs.begin(),
                working.level_runs.end(),
                target_active,
                [](const BidiLevelRun& candidate, std::uint32_t active_index) {
                    return candidate.first_active_index < active_index;
                });
            if (target_iterator == working.level_runs.end() ||
                target_iterator->first_active_index != target_active) {
                return fail(
                    BidiSequenceErrorKind::TopologyViolation,
                    last_unit_index,
                    "matching PDI does not begin a level run",
                    error);
            }
            const std::size_t target_run_index = static_cast<std::size_t>(
                std::distance(working.level_runs.begin(), target_iterator));
            if (target_run_index > static_cast<std::size_t>(kNoIndex)) {
                return fail(
                    BidiSequenceErrorKind::IndexOverflow,
                    last_unit_index,
                    "matching PDI level-run index exceeds 32-bit storage",
                    error);
            }
            const std::uint32_t target_run = static_cast<std::uint32_t>(
                target_run_index);
            if (target_run_index == run_id ||
                target_iterator->level != run.level ||
                incoming[target_run_index] != 0U) {
                return fail(
                    BidiSequenceErrorKind::TopologyViolation,
                    last_unit_index,
                    "matching PDI does not begin a unique same-level run",
                    error);
            }
            next_run[run_id] = target_run;
            incoming[target_run_index] = std::uint8_t{1U};
        }

        working.sequence_run_indices.reserve(level_run_count);
        working.sequences.reserve(level_run_count);
        for (std::size_t root = 0U; root < level_run_count; ++root) {
            if (incoming[root] != 0U) {
                continue;
            }
            if (working.sequence_run_indices.size() >
                    static_cast<std::size_t>(kNoIndex)) {
                return fail(
                    BidiSequenceErrorKind::IndexOverflow,
                    0U,
                    "sequence run-link offset exceeds 32-bit storage",
                    error);
            }

            const std::uint32_t first_link = static_cast<std::uint32_t>(
                working.sequence_run_indices.size());
            std::uint32_t current = static_cast<std::uint32_t>(root);
            std::uint32_t last_run = current;
            std::uint64_t sequence_units = 0U;
            std::uint32_t sequence_runs = 0U;
            const std::uint8_t sequence_level = working.level_runs[root].level;

            while (current != kNoIndex) {
                const std::size_t current_index = static_cast<std::size_t>(current);
                if (current_index >= level_run_count ||
                    visited[current_index] != 0U ||
                    working.level_runs[current_index].level != sequence_level) {
                    return fail(
                        BidiSequenceErrorKind::TopologyViolation,
                        0U,
                        "isolating-run-sequence graph is cyclic or inconsistent",
                        error);
                }
                visited[current_index] = std::uint8_t{1U};
                working.sequence_run_indices.push_back(current);
                ++sequence_runs;
                sequence_units += working.level_runs[current_index].active_count;
                last_run = current;
                current = next_run[current_index];
            }

            const BidiLevelRun& first_run = working.level_runs[root];
            const BidiLevelRun& final_run =
                working.level_runs[static_cast<std::size_t>(last_run)];
            const std::size_t first_active = first_run.first_active_index;
            const std::size_t last_active =
                static_cast<std::size_t>(final_run.first_active_index) +
                static_cast<std::size_t>(final_run.active_count) - 1U;
            const std::uint8_t preceding_level = first_active == 0U
                ? paragraph_level
                : units[working.active_unit_indices[first_active - 1U]].level;
            const std::uint32_t last_unit_index =
                working.active_unit_indices[last_active];
            const bool terminal_unmatched_isolate =
                is_bidi_isolate_initiator(units[last_unit_index].original_class) &&
                matching[last_active] == kNoIndex;
            const std::uint8_t following_level =
                terminal_unmatched_isolate || last_active + 1U == active_count
                ? paragraph_level
                : units[working.active_unit_indices[last_active + 1U]].level;
            const BidiClass sos = direction_for_level(std::max(
                sequence_level,
                preceding_level));
            const BidiClass eos = direction_for_level(std::max(
                sequence_level,
                following_level));

            working.sequences.push_back({
                first_link,
                sequence_runs,
                sos,
                eos,
                sequence_level,
                0U,
                0U,
            });
            stats->maximum_sequence_runs = std::max(
                stats->maximum_sequence_runs,
                static_cast<std::uint64_t>(sequence_runs));
            stats->maximum_sequence_units = std::max(
                stats->maximum_sequence_units,
                sequence_units);
        }

        for (const std::uint8_t was_visited : visited) {
            if (was_visited == 0U) {
                return fail(
                    BidiSequenceErrorKind::TopologyViolation,
                    0U,
                    "a level run is unreachable from every sequence root",
                    error);
            }
        }

        stats->isolating_sequences =
            static_cast<std::uint64_t>(working.sequences.size());
        stats->sequence_run_links =
            static_cast<std::uint64_t>(working.sequence_run_indices.size());

        topology->active_unit_indices.swap(working.active_unit_indices);
        topology->level_runs.swap(working.level_runs);
        topology->sequence_run_indices.swap(working.sequence_run_indices);
        topology->sequences.swap(working.sequences);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(
            BidiSequenceErrorKind::OutputBudgetExceeded,
            0U,
            "isolating-run-sequence construction exceeded its resource budget",
            error);
    } catch (...) {
        return fail(
            BidiSequenceErrorKind::OutputBudgetExceeded,
            0U,
            "isolating-run-sequence allocation failed",
            error);
    }
}

} // namespace zevryon::text
