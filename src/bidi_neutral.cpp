#include "bidi_neutral.hpp"

#include "unicode_bidi_brackets.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>

namespace zevryon::text {
namespace {

constexpr std::size_t kMaximumBracketStack = 63U;
constexpr std::uint8_t kContainsLeft = 1U << 0U;
constexpr std::uint8_t kContainsRight = 1U << 1U;

class SequenceCursor {
public:
    SequenceCursor(
        const BidiSequenceTopology& topology,
        const BidiIsolatingRunSequence& sequence) noexcept
        : topology_(&topology), sequence_(&sequence) {}

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

struct BracketStackEntry {
    std::uint32_t expected_closing{0};
    std::uint32_t open_active{0};
    std::uint32_t open_ordinal{0};
    BidiClass preceding_strong{BidiClass::L};
    std::uint8_t strong_flags{0};
};

struct BracketPair {
    std::uint32_t open_active{0};
    std::uint32_t close_active{0};
    std::uint32_t open_ordinal{0};
    BidiClass preceding_strong{BidiClass::L};
    std::uint8_t strong_flags{0};
    std::uint16_t reserved{0};
};

static_assert(sizeof(BracketPair) <= 16U);

void clear_error(BidiNeutralError* error) noexcept {
    if (error != nullptr) {
        error->kind = BidiNeutralErrorKind::None;
        error->active_index = 0U;
        error->message.clear();
    }
}

bool fail(
    BidiNeutralErrorKind kind,
    std::size_t active_index,
    const char* message,
    BidiNeutralError* error) noexcept {
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

BidiClass strong_direction(BidiClass value) noexcept {
    if (value == BidiClass::L) {
        return BidiClass::L;
    }
    if (value == BidiClass::R ||
        value == BidiClass::EN ||
        value == BidiClass::AN) {
        return BidiClass::R;
    }
    return BidiClass::Count;
}

bool is_neutral_or_isolate(BidiClass value) noexcept {
    return value == BidiClass::B ||
           value == BidiClass::S ||
           value == BidiClass::WS ||
           value == BidiClass::ON ||
           value == BidiClass::FSI ||
           value == BidiClass::LRI ||
           value == BidiClass::RLI ||
           value == BidiClass::PDI;
}

bool validate_topology(
    std::span<const DecodedCodePoint> codepoints,
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    std::span<const BidiClass> weak_types,
    std::pmr::memory_resource* resource,
    BidiNeutralError* error) {
    const std::size_t active_count = topology.active_unit_indices.size();
    if (weak_types.size() != active_count ||
        active_count > static_cast<std::size_t>(
                           std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            BidiNeutralErrorKind::InvalidInput,
            0U,
            "weak types do not match the 32-bit active-unit stream",
            error);
    }

    for (std::size_t active = 0U; active < active_count; ++active) {
        const std::uint32_t unit_index = topology.active_unit_indices[active];
        if (unit_index >= units.size() ||
            (active != 0U &&
             topology.active_unit_indices[active - 1U] >= unit_index) ||
            (units[unit_index].flags & kBidiUnitRemovedByX9) != 0U ||
            units[unit_index].codepoint_index >= codepoints.size() ||
            !valid_bidi_class(weak_types[active])) {
            return fail(
                BidiNeutralErrorKind::InvalidInput,
                active,
                "active indices, codepoints, or weak types are invalid",
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
                BidiNeutralErrorKind::TopologyViolation,
                expected_active,
                "level runs do not form a contiguous active partition",
                error);
        }
        const std::size_t end =
            static_cast<std::size_t>(run.first_active_index) + run.active_count;
        for (std::size_t active = run.first_active_index;
             active < end;
             ++active) {
            if (units[topology.active_unit_indices[active]].level != run.level) {
                return fail(
                    BidiNeutralErrorKind::TopologyViolation,
                    active,
                    "level-run level disagrees with explicit units",
                    error);
            }
        }
        expected_active = end;
    }
    if (expected_active != active_count ||
        topology.sequence_run_indices.size() != topology.level_runs.size()) {
        return fail(
            BidiNeutralErrorKind::TopologyViolation,
            expected_active,
            "level runs or sequence links do not cover active units",
            error);
    }

    std::pmr::vector<std::uint8_t> visited(resource);
    try {
        visited.assign(topology.level_runs.size(), std::uint8_t{0U});
    } catch (const std::bad_alloc&) {
        return fail(
            BidiNeutralErrorKind::OutputBudgetExceeded,
            0U,
            "neutral topology validation exceeded its resource budget",
            error);
    }

    std::size_t expected_link = 0U;
    for (const BidiIsolatingRunSequence& sequence : topology.sequences) {
        if (sequence.run_count == 0U ||
            sequence.first_run_link != expected_link ||
            (sequence.sos != BidiClass::L && sequence.sos != BidiClass::R) ||
            (sequence.eos != BidiClass::L && sequence.eos != BidiClass::R) ||
            static_cast<std::size_t>(sequence.first_run_link) +
                    static_cast<std::size_t>(sequence.run_count) >
                topology.sequence_run_indices.size()) {
            return fail(
                BidiNeutralErrorKind::TopologyViolation,
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
                visited[run_id] != 0U ||
                topology.level_runs[run_id].level != sequence.level) {
                return fail(
                    BidiNeutralErrorKind::TopologyViolation,
                    0U,
                    "isolating sequences duplicate or mismatch level runs",
                    error);
            }
            visited[run_id] = std::uint8_t{1U};
            const BidiLevelRun& run = topology.level_runs[run_id];
            if (have_previous && run.first_active_index <= previous_active) {
                return fail(
                    BidiNeutralErrorKind::TopologyViolation,
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
            BidiNeutralErrorKind::TopologyViolation,
            0U,
            "isolating sequences do not cover every run link",
            error);
    }
    for (const std::uint8_t value : visited) {
        if (value == 0U) {
            return fail(
                BidiNeutralErrorKind::TopologyViolation,
                0U,
                "a level run is missing from isolating sequences",
                error);
        }
    }
    return true;
}

void note_strong(
    BidiClass direction,
    std::array<BracketStackEntry, kMaximumBracketStack>* stack,
    std::size_t depth) noexcept {
    if (direction == BidiClass::Count) {
        return;
    }
    const std::uint8_t flag =
        direction == BidiClass::L ? kContainsLeft : kContainsRight;
    for (std::size_t index = 0U; index < depth; ++index) {
        (*stack)[index].strong_flags |= flag;
    }
}

bool apply_n0(
    std::span<const DecodedCodePoint> codepoints,
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    const BidiIsolatingRunSequence& sequence,
    std::pmr::vector<BidiClass>* working,
    std::pmr::vector<BidiClass>* endpoint_directions,
    BidiNeutralStats* stats) {
    const BidiClass embedding =
        (sequence.level & 1U) == 0U ? BidiClass::L : BidiClass::R;
    const BidiClass opposite =
        embedding == BidiClass::L ? BidiClass::R : BidiClass::L;
    std::array<BracketStackEntry, kMaximumBracketStack> stack{};
    std::size_t depth = 0U;
    std::uint32_t ordinal = 0U;
    BidiClass previous_strong = sequence.sos;
    std::pmr::vector<BracketPair> pairs(working->get_allocator().resource());

    for (SequenceCursor cursor(topology, sequence); cursor.valid(); cursor.advance()) {
        const std::uint32_t active = cursor.active_index();
        const BidiClass strong = strong_direction((*working)[active]);
        note_strong(strong, &stack, depth);
        if (strong != BidiClass::Count) {
            previous_strong = strong;
        }

        if ((*working)[active] != BidiClass::ON) {
            ++ordinal;
            continue;
        }
        const BidiExplicitUnit& unit = units[topology.active_unit_indices[active]];
        const std::uint32_t codepoint = codepoints[unit.codepoint_index].value;
        const BidiBracketInfo bracket = bidi_bracket_info(codepoint);
        if (bracket.type == BidiBracketType::None) {
            ++ordinal;
            continue;
        }
        ++stats->bracket_candidates;
        if (bracket.type == BidiBracketType::Open) {
            if (depth == stack.size()) {
                pairs.clear();
                ++stats->bracket_overflow_sequences;
                return true;
            }
            stack[depth] = BracketStackEntry{
                bracket.paired_codepoint,
                active,
                ordinal,
                previous_strong,
                0U};
            ++depth;
            ++ordinal;
            continue;
        }

        std::size_t match = depth;
        for (std::size_t index = depth; index > 0U; --index) {
            const std::size_t candidate = index - 1U;
            if (bidi_brackets_match(stack[candidate].expected_closing, codepoint)) {
                match = candidate;
                break;
            }
        }
        if (match != depth) {
            const BracketStackEntry& opening = stack[match];
            pairs.push_back(BracketPair{
                opening.open_active,
                active,
                opening.open_ordinal,
                opening.preceding_strong,
                opening.strong_flags,
                0U});
            depth = match;
        }
        ++ordinal;
    }

    stats->bracket_pairs += pairs.size();
    std::sort(
        pairs.begin(),
        pairs.end(),
        [](const BracketPair& left, const BracketPair& right) {
            return left.open_ordinal < right.open_ordinal;
        });

    for (const BracketPair& pair : pairs) {
        BidiClass resolved = BidiClass::Count;
        const std::uint8_t embedding_flag =
            embedding == BidiClass::L ? kContainsLeft : kContainsRight;
        const std::uint8_t opposite_flag =
            opposite == BidiClass::L ? kContainsLeft : kContainsRight;
        if ((pair.strong_flags & embedding_flag) != 0U) {
            resolved = embedding;
            ++stats->n0_embedding_pairs;
        } else if ((pair.strong_flags & opposite_flag) != 0U) {
            if (pair.preceding_strong == opposite) {
                resolved = opposite;
                ++stats->n0_opposite_pairs;
            } else {
                resolved = embedding;
                ++stats->n0_embedding_pairs;
            }
        }
        if (resolved != BidiClass::Count) {
            (*endpoint_directions)[pair.open_active] = resolved;
            (*endpoint_directions)[pair.close_active] = resolved;
        }
    }

    BidiClass following_nsm_direction = BidiClass::Count;
    for (SequenceCursor cursor(topology, sequence); cursor.valid(); cursor.advance()) {
        const std::uint32_t active = cursor.active_index();
        const BidiClass endpoint = (*endpoint_directions)[active];
        if (endpoint != BidiClass::Count) {
            (*working)[active] = endpoint;
            following_nsm_direction = endpoint;
            continue;
        }
        const BidiExplicitUnit& unit = units[topology.active_unit_indices[active]];
        if (unit.original_class == BidiClass::NSM &&
            following_nsm_direction != BidiClass::Count) {
            if ((*working)[active] != following_nsm_direction) {
                (*working)[active] = following_nsm_direction;
                ++stats->n0_following_nsm_changes;
            }
        } else {
            following_nsm_direction = BidiClass::Count;
        }
    }
    return true;
}

bool apply_n1_n2(
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    const BidiIsolatingRunSequence& sequence,
    std::pmr::vector<BidiClass>* working,
    BidiNeutralStats* stats,
    BidiNeutralError* error) {
    BidiClass left = sequence.sos;
    SequenceCursor cursor(topology, sequence);
    while (cursor.valid()) {
        const std::uint32_t active = cursor.active_index();
        const BidiClass current = (*working)[active];
        if (!is_neutral_or_isolate(current)) {
            const BidiClass direction = strong_direction(current);
            if (direction == BidiClass::Count) {
                return fail(
                    BidiNeutralErrorKind::InvalidInput,
                    active,
                    "weak types contain an unresolved non-neutral class",
                    error);
            }
            left = direction;
            cursor.advance();
            continue;
        }

        const SequenceCursor first = cursor;
        SequenceCursor after = cursor;
        while (after.valid() &&
               is_neutral_or_isolate((*working)[after.active_index()])) {
            after.advance();
        }
        const BidiClass right = after.valid()
            ? strong_direction((*working)[after.active_index()])
            : sequence.eos;
        if (right == BidiClass::Count) {
            return fail(
                BidiNeutralErrorKind::InvalidInput,
                after.valid() ? after.active_index() : active,
                "neutral sequence has an invalid strong boundary",
                error);
        }

        SequenceCursor mark = first;
        if (left == right) {
            while (!(mark == after)) {
                const std::uint32_t index = mark.active_index();
                if ((*working)[index] != left) {
                    (*working)[index] = left;
                    ++stats->n1_changes;
                }
                mark.advance();
            }
        } else {
            while (!(mark == after)) {
                const std::uint32_t index = mark.active_index();
                const BidiExplicitUnit& unit =
                    units[topology.active_unit_indices[index]];
                const BidiClass direction =
                    (unit.level & 1U) == 0U ? BidiClass::L : BidiClass::R;
                if ((*working)[index] != direction) {
                    (*working)[index] = direction;
                    ++stats->n2_changes;
                }
                mark.advance();
            }
        }
        left = right;
        cursor = after;
    }
    return true;
}

} // namespace

const char* bidi_neutral_error_kind_name(BidiNeutralErrorKind kind) noexcept {
    switch (kind) {
        case BidiNeutralErrorKind::None:
            return "none";
        case BidiNeutralErrorKind::InvalidInput:
            return "invalid_input";
        case BidiNeutralErrorKind::TopologyViolation:
            return "topology_violation";
        case BidiNeutralErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
    }
    return "invalid";
}

bool resolve_bidi_neutral_types(
    std::span<const DecodedCodePoint> codepoints,
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    std::span<const BidiClass> weak_types,
    std::pmr::vector<BidiClass>* resolved_types,
    BidiNeutralStats* stats,
    BidiNeutralError* error) noexcept {
    if (resolved_types == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    clear_error(error);
    release_vector(resolved_types);
    *stats = {};
    stats->input_units = static_cast<std::uint64_t>(units.size());

    try {
        if (!validate_topology(
                codepoints,
                units,
                topology,
                weak_types,
                resolved_types->get_allocator().resource(),
                error)) {
            return false;
        }

        std::pmr::vector<BidiClass> working(
            resolved_types->get_allocator().resource());
        working.assign(weak_types.begin(), weak_types.end());
        std::pmr::vector<BidiClass> endpoint_directions(
            resolved_types->get_allocator().resource());
        endpoint_directions.assign(weak_types.size(), BidiClass::Count);

        stats->active_units = static_cast<std::uint64_t>(working.size());
        stats->isolating_sequences =
            static_cast<std::uint64_t>(topology.sequences.size());
        for (const BidiIsolatingRunSequence& sequence : topology.sequences) {
            if (!apply_n0(
                    codepoints,
                    units,
                    topology,
                    sequence,
                    &working,
                    &endpoint_directions,
                    stats) ||
                !apply_n1_n2(
                    units,
                    topology,
                    sequence,
                    &working,
                    stats,
                    error)) {
                return false;
            }
        }

        stats->output_types = static_cast<std::uint64_t>(working.size());
        resolved_types->swap(working);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(
            BidiNeutralErrorKind::OutputBudgetExceeded,
            0U,
            "neutral resolution exceeded its resource budget",
            error);
    } catch (...) {
        return fail(
            BidiNeutralErrorKind::OutputBudgetExceeded,
            0U,
            "neutral resolution allocation failed",
            error);
    }
}

} // namespace zevryon::text
