#include "bidi_weak.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace zevryon::text {
namespace {

class SequenceCursor {
public:
    SequenceCursor(
        const BidiSequenceTopology& topology,
        const BidiIsolatingRunSequence& sequence) noexcept
        : topology_(&topology),
          sequence_(&sequence) {}

    bool valid() const noexcept {
        return run_offset_ < sequence_->run_count;
    }

    std::uint32_t active_index() const noexcept {
        const std::size_t link =
            static_cast<std::size_t>(sequence_->first_run_link) + run_offset_;
        const BidiLevelRun& run =
            topology_->level_runs[topology_->sequence_run_indices[link]];
        return static_cast<std::uint32_t>(
            static_cast<std::size_t>(run.first_active_index) + unit_offset_);
    }

    void advance() noexcept {
        if (!valid()) {
            return;
        }
        const std::size_t link =
            static_cast<std::size_t>(sequence_->first_run_link) + run_offset_;
        const BidiLevelRun& run =
            topology_->level_runs[topology_->sequence_run_indices[link]];
        ++unit_offset_;
        if (unit_offset_ >= run.active_count) {
            unit_offset_ = 0U;
            ++run_offset_;
        }
    }

    bool operator==(const SequenceCursor& other) const noexcept {
        return topology_ == other.topology_ &&
               sequence_ == other.sequence_ &&
               run_offset_ == other.run_offset_ &&
               unit_offset_ == other.unit_offset_;
    }

private:
    const BidiSequenceTopology* topology_{nullptr};
    const BidiIsolatingRunSequence* sequence_{nullptr};
    std::size_t run_offset_{0};
    std::size_t unit_offset_{0};
};

void clear_error(BidiWeakError* error) noexcept {
    if (error != nullptr) {
        error->kind = BidiWeakErrorKind::None;
        error->active_index = 0U;
        error->message.clear();
    }
}

bool fail(
    BidiWeakErrorKind kind,
    std::size_t active_index,
    const char* message,
    BidiWeakError* error) noexcept {
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

bool valid_bidi_class(BidiClass value) noexcept {
    return static_cast<std::size_t>(value) <
           static_cast<std::size_t>(BidiClass::Count);
}

bool isolate_boundary(BidiClass value) noexcept {
    return is_bidi_isolate_initiator(value) || value == BidiClass::PDI;
}

template <typename T>
void release_vector(std::pmr::vector<T>* values) noexcept {
    std::pmr::vector<T> empty(values->get_allocator().resource());
    values->swap(empty);
}

bool validate_topology(
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    std::pmr::memory_resource* resource,
    BidiWeakError* error) {
    const std::size_t active_count = topology.active_unit_indices.size();
    if (active_count > static_cast<std::size_t>(
                           std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            BidiWeakErrorKind::InvalidInput,
            0U,
            "active bidi unit count exceeds 32-bit storage",
            error);
    }

    for (std::size_t active = 0U; active < active_count; ++active) {
        const std::uint32_t unit_index = topology.active_unit_indices[active];
        if (unit_index >= units.size() ||
            (active != 0U &&
             topology.active_unit_indices[active - 1U] >= unit_index) ||
            (units[unit_index].flags & kBidiUnitRemovedByX9) != 0U ||
            !valid_bidi_class(units[unit_index].resolved_class)) {
            return fail(
                BidiWeakErrorKind::InvalidInput,
                active,
                "active bidi indices do not match valid X9-filtered units",
                error);
        }
    }

    std::size_t expected_active = 0U;
    for (std::size_t run_index = 0U;
         run_index < topology.level_runs.size();
         ++run_index) {
        const BidiLevelRun& run = topology.level_runs[run_index];
        if (run.active_count == 0U ||
            run.first_active_index != expected_active ||
            static_cast<std::size_t>(run.first_active_index) +
                    static_cast<std::size_t>(run.active_count) >
                active_count) {
            return fail(
                BidiWeakErrorKind::TopologyViolation,
                expected_active,
                "level runs do not form a contiguous active-unit partition",
                error);
        }
        const std::size_t end =
            static_cast<std::size_t>(run.first_active_index) + run.active_count;
        for (std::size_t active = run.first_active_index;
             active < end;
             ++active) {
            if (units[topology.active_unit_indices[active]].level != run.level) {
                return fail(
                    BidiWeakErrorKind::TopologyViolation,
                    active,
                    "level-run level disagrees with explicit bidi units",
                    error);
            }
        }
        expected_active = end;
    }
    if (expected_active != active_count) {
        return fail(
            BidiWeakErrorKind::TopologyViolation,
            expected_active,
            "level runs do not cover every active unit",
            error);
    }

    if (topology.sequence_run_indices.size() != topology.level_runs.size()) {
        return fail(
            BidiWeakErrorKind::TopologyViolation,
            0U,
            "sequence run links do not cover every level run",
            error);
    }

    std::pmr::vector<std::uint8_t> visited(resource);
    try {
        visited.assign(topology.level_runs.size(), std::uint8_t{0U});
    } catch (const std::bad_alloc&) {
        return fail(
            BidiWeakErrorKind::OutputBudgetExceeded,
            0U,
            "weak-type topology validation exceeded its resource budget",
            error);
    }

    std::size_t expected_link = 0U;
    for (const BidiIsolatingRunSequence& sequence : topology.sequences) {
        if (sequence.run_count == 0U ||
            sequence.first_run_link != expected_link ||
            sequence.sos != BidiClass::L && sequence.sos != BidiClass::R ||
            sequence.eos != BidiClass::L && sequence.eos != BidiClass::R ||
            static_cast<std::size_t>(sequence.first_run_link) +
                    static_cast<std::size_t>(sequence.run_count) >
                topology.sequence_run_indices.size()) {
            return fail(
                BidiWeakErrorKind::TopologyViolation,
                0U,
                "isolating sequence descriptors are not contiguous or valid",
                error);
        }
        for (std::size_t offset = 0U; offset < sequence.run_count; ++offset) {
            const std::size_t link =
                static_cast<std::size_t>(sequence.first_run_link) + offset;
            const std::uint32_t run_id = topology.sequence_run_indices[link];
            if (run_id >= topology.level_runs.size() ||
                visited[run_id] != 0U ||
                topology.level_runs[run_id].level != sequence.level) {
                return fail(
                    BidiWeakErrorKind::TopologyViolation,
                    0U,
                    "isolating sequences duplicate or mismatch level runs",
                    error);
            }
            visited[run_id] = std::uint8_t{1U};
        }
        expected_link += sequence.run_count;
    }
    if (expected_link != topology.sequence_run_indices.size()) {
        return fail(
            BidiWeakErrorKind::TopologyViolation,
            0U,
            "isolating sequences do not cover every run link",
            error);
    }
    for (const std::uint8_t value : visited) {
        if (value == 0U) {
            return fail(
                BidiWeakErrorKind::TopologyViolation,
                0U,
                "a level run is missing from isolating sequences",
                error);
        }
    }
    return true;
}

void apply_w1(
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    const BidiIsolatingRunSequence& sequence,
    std::pmr::vector<BidiClass>* types,
    BidiWeakStats* stats) noexcept {
    BidiClass previous = sequence.sos;
    bool previous_is_isolate_boundary = false;
    for (SequenceCursor cursor(topology, sequence); cursor.valid(); cursor.advance()) {
        const std::uint32_t active = cursor.active_index();
        BidiClass current = (*types)[active];
        if (current == BidiClass::NSM) {
            current = previous_is_isolate_boundary ? BidiClass::ON : previous;
            (*types)[active] = current;
            ++stats->w1_nsm_changes;
        }
        const BidiClass original =
            units[topology.active_unit_indices[active]].original_class;
        previous = current;
        previous_is_isolate_boundary = isolate_boundary(original);
    }
}

void apply_w2(
    const BidiSequenceTopology& topology,
    const BidiIsolatingRunSequence& sequence,
    std::pmr::vector<BidiClass>* types,
    BidiWeakStats* stats) noexcept {
    BidiClass last_strong = sequence.sos;
    for (SequenceCursor cursor(topology, sequence); cursor.valid(); cursor.advance()) {
        const std::uint32_t active = cursor.active_index();
        const BidiClass current = (*types)[active];
        if (current == BidiClass::EN && last_strong == BidiClass::AL) {
            (*types)[active] = BidiClass::AN;
            ++stats->w2_en_to_an;
        }
        if (current == BidiClass::R ||
            current == BidiClass::L ||
            current == BidiClass::AL) {
            last_strong = current;
        }
    }
}

void apply_w3(
    const BidiSequenceTopology& topology,
    const BidiIsolatingRunSequence& sequence,
    std::pmr::vector<BidiClass>* types,
    BidiWeakStats* stats) noexcept {
    for (SequenceCursor cursor(topology, sequence); cursor.valid(); cursor.advance()) {
        const std::uint32_t active = cursor.active_index();
        if ((*types)[active] == BidiClass::AL) {
            (*types)[active] = BidiClass::R;
            ++stats->w3_al_to_r;
        }
    }
}

void apply_w4(
    const BidiSequenceTopology& topology,
    const BidiIsolatingRunSequence& sequence,
    std::pmr::vector<BidiClass>* types,
    BidiWeakStats* stats) noexcept {
    SequenceCursor cursor(topology, sequence);
    if (!cursor.valid()) {
        return;
    }
    BidiClass previous = sequence.sos;
    while (cursor.valid()) {
        const std::uint32_t active = cursor.active_index();
        const BidiClass current = (*types)[active];
        SequenceCursor next = cursor;
        next.advance();
        const BidiClass following = next.valid()
            ? (*types)[next.active_index()]
            : sequence.eos;
        if (current == BidiClass::ES &&
            previous == BidiClass::EN &&
            following == BidiClass::EN) {
            (*types)[active] = BidiClass::EN;
            ++stats->w4_separator_changes;
        } else if (current == BidiClass::CS &&
                   previous == following &&
                   (previous == BidiClass::EN || previous == BidiClass::AN)) {
            (*types)[active] = previous;
            ++stats->w4_separator_changes;
        }
        previous = (*types)[active];
        cursor.advance();
    }
}

void apply_w5(
    const BidiSequenceTopology& topology,
    const BidiIsolatingRunSequence& sequence,
    std::pmr::vector<BidiClass>* types,
    BidiWeakStats* stats) noexcept {
    SequenceCursor cursor(topology, sequence);
    BidiClass previous = sequence.sos;
    while (cursor.valid()) {
        const std::uint32_t active = cursor.active_index();
        if ((*types)[active] != BidiClass::ET) {
            previous = (*types)[active];
            cursor.advance();
            continue;
        }

        const SequenceCursor first_et = cursor;
        SequenceCursor after_et = cursor;
        std::uint64_t et_count = 0U;
        while (after_et.valid() &&
               (*types)[after_et.active_index()] == BidiClass::ET) {
            ++et_count;
            after_et.advance();
        }
        const BidiClass following = after_et.valid()
            ? (*types)[after_et.active_index()]
            : sequence.eos;
        const bool convert = previous == BidiClass::EN || following == BidiClass::EN;
        if (convert) {
            SequenceCursor mark = first_et;
            while (!(mark == after_et)) {
                (*types)[mark.active_index()] = BidiClass::EN;
                mark.advance();
            }
            stats->w5_et_to_en += et_count;
            previous = BidiClass::EN;
        } else {
            previous = BidiClass::ET;
        }
        cursor = after_et;
    }
}

void apply_w6(
    const BidiSequenceTopology& topology,
    const BidiIsolatingRunSequence& sequence,
    std::pmr::vector<BidiClass>* types,
    BidiWeakStats* stats) noexcept {
    for (SequenceCursor cursor(topology, sequence); cursor.valid(); cursor.advance()) {
        const std::uint32_t active = cursor.active_index();
        const BidiClass current = (*types)[active];
        if (current == BidiClass::ES ||
            current == BidiClass::ET ||
            current == BidiClass::CS) {
            (*types)[active] = BidiClass::ON;
            ++stats->w6_neutralized;
        }
    }
}

void apply_w7(
    const BidiSequenceTopology& topology,
    const BidiIsolatingRunSequence& sequence,
    std::pmr::vector<BidiClass>* types,
    BidiWeakStats* stats) noexcept {
    BidiClass last_strong = sequence.sos;
    for (SequenceCursor cursor(topology, sequence); cursor.valid(); cursor.advance()) {
        const std::uint32_t active = cursor.active_index();
        const BidiClass current = (*types)[active];
        if (current == BidiClass::EN && last_strong == BidiClass::L) {
            (*types)[active] = BidiClass::L;
            ++stats->w7_en_to_l;
        }
        if (current == BidiClass::L || current == BidiClass::R) {
            last_strong = current;
        }
    }
}

} // namespace

const char* bidi_weak_error_kind_name(BidiWeakErrorKind kind) noexcept {
    switch (kind) {
        case BidiWeakErrorKind::None:
            return "none";
        case BidiWeakErrorKind::InvalidInput:
            return "invalid_input";
        case BidiWeakErrorKind::TopologyViolation:
            return "topology_violation";
        case BidiWeakErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
    }
    return "invalid";
}

bool resolve_bidi_weak_types(
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    std::pmr::vector<BidiClass>* resolved_types,
    BidiWeakStats* stats,
    BidiWeakError* error) noexcept {
    if (resolved_types == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    clear_error(error);
    release_vector(resolved_types);
    *stats = {};
    stats->input_units = static_cast<std::uint64_t>(units.size());

    try {
        if (!validate_topology(
                units,
                topology,
                resolved_types->get_allocator().resource(),
                error)) {
            return false;
        }

        std::pmr::vector<BidiClass> working(
            resolved_types->get_allocator().resource());
        working.reserve(topology.active_unit_indices.size());
        for (const std::uint32_t unit_index : topology.active_unit_indices) {
            working.push_back(units[unit_index].resolved_class);
        }

        stats->active_units = static_cast<std::uint64_t>(working.size());
        stats->isolating_sequences =
            static_cast<std::uint64_t>(topology.sequences.size());
        for (const BidiIsolatingRunSequence& sequence : topology.sequences) {
            apply_w1(units, topology, sequence, &working, stats);
            apply_w2(topology, sequence, &working, stats);
            apply_w3(topology, sequence, &working, stats);
            apply_w4(topology, sequence, &working, stats);
            apply_w5(topology, sequence, &working, stats);
            apply_w6(topology, sequence, &working, stats);
            apply_w7(topology, sequence, &working, stats);
        }

        stats->output_types = static_cast<std::uint64_t>(working.size());
        resolved_types->swap(working);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(
            BidiWeakErrorKind::OutputBudgetExceeded,
            0U,
            "weak-type resolution exceeded its resource budget",
            error);
    } catch (...) {
        return fail(
            BidiWeakErrorKind::OutputBudgetExceeded,
            0U,
            "weak-type resolution allocation failed",
            error);
    }
}

} // namespace zevryon::text
