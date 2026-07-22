#include "bidi_resolver.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <new>
#include <utility>

namespace zevryon::text {
namespace {

constexpr std::uint8_t kFlagHasCodepoint = 1U << 0U;
constexpr std::uint8_t kFlagNumeric = 1U << 1U;
constexpr std::uint8_t kFlagRemoved = 1U << 2U;
constexpr std::int32_t kNoMatch = -1;
constexpr std::uint8_t kMaximumExplicitLevel = 125U;
constexpr std::size_t kMaximumBracketDepth = 63U;

struct Unit {
    std::uint32_t codepoint{0};
    BidiClass original{BidiClass::L};
    BidiClass current{BidiClass::L};
    std::uint8_t level{0};
    std::uint8_t flags{0};
};

static_assert(sizeof(Unit) <= 8U, "bidi working units must remain compact");

struct StatusEntry {
    std::uint8_t level{0};
    BidiClass override_value{BidiClass::ON};
    bool isolate{false};
};

struct LevelRun {
    std::uint32_t visible_start{0};
    std::uint32_t visible_end{0};
    std::int32_t next{kNoMatch};
    std::uint8_t level{0};
    BidiClass sor{BidiClass::L};
    BidiClass eor{BidiClass::L};
    bool incoming{false};
};

struct BracketStackEntry {
    std::uint32_t expected_closing{0};
    std::uint32_t sequence_position{0};
};

struct BracketPair {
    std::uint32_t opening_position{0};
    std::uint32_t closing_position{0};
};

bool has_flag(const Unit& unit, std::uint8_t flag) noexcept {
    return (unit.flags & flag) != 0U;
}

void set_flag(Unit* unit, std::uint8_t flag, bool value) noexcept {
    if (value) {
        unit->flags = static_cast<std::uint8_t>(unit->flags | flag);
    } else {
        unit->flags = static_cast<std::uint8_t>(unit->flags & ~flag);
    }
}

bool valid_bidi_class(BidiClass value) noexcept {
    return static_cast<std::size_t>(value) <
           static_cast<std::size_t>(BidiClass::Count);
}

void clear_error(BidiError* error) noexcept {
    if (error != nullptr) {
        error->kind = BidiErrorKind::None;
        error->input_index = 0U;
        error->message.clear();
    }
}

bool fail(
    BidiErrorKind kind,
    std::size_t input_index,
    const char* message,
    BidiError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->input_index = input_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

BidiClass direction_from_level(std::uint8_t level) noexcept {
    return (level % 2U) == 0U ? BidiClass::L : BidiClass::R;
}

int least_greater_odd_level(std::uint8_t level) noexcept {
    const int candidate = (static_cast<int>(level) + 1) | 1;
    return candidate <= static_cast<int>(kMaximumExplicitLevel)
        ? candidate
        : -1;
}

int least_greater_even_level(std::uint8_t level) noexcept {
    const int candidate = (static_cast<int>(level) + 2) & ~1;
    return candidate <= static_cast<int>(kMaximumExplicitLevel)
        ? candidate
        : -1;
}

bool override_active(const StatusEntry& entry) noexcept {
    return entry.override_value == BidiClass::L ||
           entry.override_value == BidiClass::R;
}

bool neutral_type(BidiClass value) noexcept {
    return value == BidiClass::B || value == BidiClass::S ||
           value == BidiClass::WS || value == BidiClass::ON ||
           value == BidiClass::LRI || value == BidiClass::RLI ||
           value == BidiClass::FSI || value == BidiClass::PDI;
}

BidiClass strong_for_neutral(const Unit& unit) noexcept {
    if (unit.current == BidiClass::L) {
        return BidiClass::L;
    }
    if (unit.current == BidiClass::R ||
        unit.current == BidiClass::EN ||
        unit.current == BidiClass::AN) {
        return BidiClass::R;
    }
    return BidiClass::ON;
}

BidiClass strong_for_bracket(const Unit& unit) noexcept {
    if (unit.current == BidiClass::R || has_flag(unit, kFlagNumeric)) {
        return BidiClass::R;
    }
    if (unit.current == BidiClass::L) {
        return BidiClass::L;
    }
    return BidiClass::ON;
}

bool match_isolates(
    const std::pmr::vector<Unit>& units,
    std::pmr::vector<std::int32_t>* matching_pdi,
    std::pmr::memory_resource* memory,
    BidiError* error) noexcept {
    try {
        matching_pdi->assign(units.size(), kNoMatch);
        std::pmr::vector<std::uint32_t> stack(memory);
        stack.reserve(units.size());
        for (std::size_t index = 0U; index < units.size(); ++index) {
            const BidiClass value = units[index].original;
            if (is_isolate_initiator(value)) {
                stack.push_back(static_cast<std::uint32_t>(index));
            } else if (value == BidiClass::PDI && !stack.empty()) {
                const std::uint32_t initiator = stack.back();
                stack.pop_back();
                (*matching_pdi)[initiator] = static_cast<std::int32_t>(index);
            }
        }
        return true;
    } catch (const std::bad_alloc&) {
        return fail(
            BidiErrorKind::OutputBudgetExceeded,
            0U,
            "bidi isolate matching exceeded its resource budget",
            error);
    } catch (...) {
        return fail(
            BidiErrorKind::InternalInvariant,
            0U,
            "bidi isolate matching failed",
            error);
    }
}

std::uint8_t decide_paragraph_level(
    const std::pmr::vector<Unit>& units,
    std::size_t first,
    std::size_t end,
    const std::pmr::vector<std::int32_t>& matching_pdi) noexcept {
    std::size_t index = first;
    while (index < end) {
        const BidiClass value = units[index].original;
        if (value == BidiClass::L) {
            return 0U;
        }
        if (value == BidiClass::R || value == BidiClass::AL) {
            return 1U;
        }
        if (is_isolate_initiator(value)) {
            const std::int32_t match = matching_pdi[index];
            if (match >= 0 && static_cast<std::size_t>(match) < end) {
                index = static_cast<std::size_t>(match) + 1U;
                continue;
            }
            return 0U;
        }
        ++index;
    }
    return 0U;
}

void apply_override(Unit* unit, const StatusEntry& entry, BidiStats* stats) noexcept {
    if (override_active(entry)) {
        if (unit->current != entry.override_value) {
            ++stats->override_changes;
        }
        unit->current = entry.override_value;
        set_flag(unit, kFlagNumeric, false);
    }
}

void apply_explicit_levels(
    std::pmr::vector<Unit>* units,
    std::uint8_t paragraph_level,
    const std::pmr::vector<std::int32_t>& matching_pdi,
    BidiStats* stats) noexcept {
    std::array<StatusEntry, 128U> stack{};
    std::size_t stack_size = 1U;
    stack[0] = {paragraph_level, BidiClass::ON, false};
    std::uint32_t overflow_isolate_count = 0U;
    std::uint32_t overflow_embedding_count = 0U;
    std::uint32_t valid_isolate_count = 0U;

    for (std::size_t index = 0U; index < units->size(); ++index) {
        Unit& unit = (*units)[index];
        const BidiClass original = unit.original;
        const StatusEntry current = stack[stack_size - 1U];
        if (original == BidiClass::RLE || original == BidiClass::LRE ||
            original == BidiClass::RLO || original == BidiClass::LRO) {
            const int new_level =
                original == BidiClass::RLE || original == BidiClass::RLO
                    ? least_greater_odd_level(current.level)
                    : least_greater_even_level(current.level);
            BidiClass override_value = BidiClass::ON;
            if (original == BidiClass::RLO) {
                override_value = BidiClass::R;
            } else if (original == BidiClass::LRO) {
                override_value = BidiClass::L;
            }
            if (new_level >= 0 && overflow_isolate_count == 0U &&
                overflow_embedding_count == 0U && stack_size < stack.size()) {
                stack[stack_size++] = {
                    static_cast<std::uint8_t>(new_level),
                    override_value,
                    false,
                };
            } else if (overflow_isolate_count == 0U) {
                ++overflow_embedding_count;
                ++stats->embedding_overflows;
            }
            continue;
        }

        if (is_isolate_initiator(original)) {
            unit.level = current.level;
            apply_override(&unit, current, stats);
            int new_level = -1;
            if (original == BidiClass::RLI) {
                new_level = least_greater_odd_level(current.level);
            } else if (original == BidiClass::LRI) {
                new_level = least_greater_even_level(current.level);
            } else {
                const std::int32_t match = matching_pdi[index];
                const std::size_t end = match >= 0
                    ? static_cast<std::size_t>(match)
                    : units->size();
                const std::uint8_t fsi_level = decide_paragraph_level(
                    *units,
                    index + 1U,
                    end,
                    matching_pdi);
                new_level = fsi_level == 1U
                    ? least_greater_odd_level(current.level)
                    : least_greater_even_level(current.level);
            }
            if (new_level >= 0 && overflow_isolate_count == 0U &&
                overflow_embedding_count == 0U && stack_size < stack.size()) {
                ++valid_isolate_count;
                stack[stack_size++] = {
                    static_cast<std::uint8_t>(new_level),
                    BidiClass::ON,
                    true,
                };
            } else {
                ++overflow_isolate_count;
                ++stats->isolate_overflows;
            }
            continue;
        }

        if (original == BidiClass::PDI) {
            if (overflow_isolate_count > 0U) {
                --overflow_isolate_count;
            } else if (valid_isolate_count != 0U) {
                overflow_embedding_count = 0U;
                while (stack_size > 1U && !stack[stack_size - 1U].isolate) {
                    --stack_size;
                }
                if (stack_size > 1U && stack[stack_size - 1U].isolate) {
                    --stack_size;
                }
                --valid_isolate_count;
            }
            const StatusEntry after_pop = stack[stack_size - 1U];
            unit.level = after_pop.level;
            apply_override(&unit, after_pop, stats);
            continue;
        }

        if (original == BidiClass::PDF) {
            if (overflow_isolate_count > 0U) {
                continue;
            }
            if (overflow_embedding_count > 0U) {
                --overflow_embedding_count;
                continue;
            }
            if (stack_size >= 2U && !stack[stack_size - 1U].isolate) {
                --stack_size;
            }
            continue;
        }

        if (original == BidiClass::BN) {
            continue;
        }
        if (original == BidiClass::B) {
            unit.level = paragraph_level;
            continue;
        }
        unit.level = current.level;
        apply_override(&unit, current, stats);
    }

    for (Unit& unit : *units) {
        if (is_removed_by_x9(unit.original)) {
            unit.level = kBidiRemovedLevel;
            set_flag(&unit, kFlagRemoved, true);
            ++stats->removed_units;
        }
    }
}

bool build_level_runs(
    const std::pmr::vector<Unit>& units,
    std::uint8_t paragraph_level,
    const std::pmr::vector<std::int32_t>& matching_pdi,
    std::pmr::vector<std::uint32_t>* visible,
    std::pmr::vector<LevelRun>* runs,
    std::pmr::vector<std::int32_t>* index_to_run,
    BidiStats* stats,
    BidiError* error) noexcept {
    try {
        visible->clear();
        visible->reserve(units.size());
        index_to_run->assign(units.size(), kNoMatch);
        for (std::size_t index = 0U; index < units.size(); ++index) {
            if (!has_flag(units[index], kFlagRemoved)) {
                visible->push_back(static_cast<std::uint32_t>(index));
            }
        }
        runs->clear();
        std::size_t position = 0U;
        while (position < visible->size()) {
            const std::size_t start = position;
            const std::uint8_t level = units[(*visible)[position]].level;
            while (position < visible->size()) {
                const std::uint32_t index = (*visible)[position];
                if (position != start && units[index].level != level) {
                    break;
                }
                ++position;
                if (is_isolate_initiator(units[index].original)) {
                    break;
                }
            }
            if (start > static_cast<std::size_t>(
                            std::numeric_limits<std::uint32_t>::max()) ||
                position > static_cast<std::size_t>(
                               std::numeric_limits<std::uint32_t>::max())) {
                return fail(
                    BidiErrorKind::IndexOverflow,
                    start,
                    "bidi level-run index exceeds 32-bit storage",
                    error);
            }
            runs->push_back({
                static_cast<std::uint32_t>(start),
                static_cast<std::uint32_t>(position),
                kNoMatch,
                level,
                BidiClass::L,
                BidiClass::L,
                false,
            });
        }

        for (std::size_t run_index = 0U; run_index < runs->size(); ++run_index) {
            LevelRun& run = (*runs)[run_index];
            for (std::size_t position_index = run.visible_start;
                 position_index < run.visible_end;
                 ++position_index) {
                (*index_to_run)[(*visible)[position_index]] =
                    static_cast<std::int32_t>(run_index);
            }
            const std::uint8_t prior_level = run_index == 0U
                ? paragraph_level
                : (*runs)[run_index - 1U].level;
            const std::uint8_t next_level = run_index + 1U == runs->size()
                ? paragraph_level
                : (*runs)[run_index + 1U].level;
            run.sor = direction_from_level(std::max(prior_level, run.level));
            run.eor = direction_from_level(std::max(next_level, run.level));
        }

        for (std::size_t run_index = 0U; run_index < runs->size(); ++run_index) {
            LevelRun& run = (*runs)[run_index];
            const std::uint32_t last_index =
                (*visible)[static_cast<std::size_t>(run.visible_end) - 1U];
            if (!is_isolate_initiator(units[last_index].original)) {
                continue;
            }
            const std::int32_t matching_index = matching_pdi[last_index];
            if (matching_index < 0) {
                continue;
            }
            const std::int32_t next_run =
                (*index_to_run)[static_cast<std::size_t>(matching_index)];
            if (next_run >= 0 &&
                (*runs)[static_cast<std::size_t>(next_run)].level == run.level) {
                run.next = next_run;
                (*runs)[static_cast<std::size_t>(next_run)].incoming = true;
            }
        }
        stats->level_runs = static_cast<std::uint64_t>(runs->size());
        return true;
    } catch (const std::bad_alloc&) {
        return fail(
            BidiErrorKind::OutputBudgetExceeded,
            0U,
            "bidi level-run construction exceeded its resource budget",
            error);
    } catch (...) {
        return fail(
            BidiErrorKind::InternalInvariant,
            0U,
            "bidi level-run construction failed",
            error);
    }
}

void resolve_weak_types(
    std::pmr::vector<Unit>* units,
    const std::pmr::vector<std::uint32_t>& sequence,
    BidiClass sos) noexcept {
    BidiClass prior = sos;
    for (const std::uint32_t index : sequence) {
        Unit& unit = (*units)[index];
        if (unit.current == BidiClass::NSM) {
            unit.current = is_isolate_initiator(prior) || prior == BidiClass::PDI
                ? BidiClass::ON
                : prior;
            if (unit.current == BidiClass::AN || unit.current == BidiClass::EN) {
                set_flag(&unit, kFlagNumeric, true);
            }
        }
        prior = unit.current;
    }

    BidiClass last_strong = sos;
    for (const std::uint32_t index : sequence) {
        Unit& unit = (*units)[index];
        if (unit.current == BidiClass::L || unit.current == BidiClass::R ||
            unit.current == BidiClass::AL) {
            last_strong = unit.current;
        } else if (unit.current == BidiClass::EN && last_strong == BidiClass::AL) {
            unit.current = BidiClass::AN;
            set_flag(&unit, kFlagNumeric, true);
        }
    }

    for (const std::uint32_t index : sequence) {
        Unit& unit = (*units)[index];
        if (unit.current == BidiClass::AL) {
            unit.current = BidiClass::R;
        }
    }

    if (sequence.size() >= 3U) {
        for (std::size_t position = 1U; position + 1U < sequence.size(); ++position) {
            Unit& unit = (*units)[sequence[position]];
            const BidiClass before = (*units)[sequence[position - 1U]].current;
            const BidiClass after = (*units)[sequence[position + 1U]].current;
            if (unit.current == BidiClass::ES &&
                before == BidiClass::EN && after == BidiClass::EN) {
                unit.current = BidiClass::EN;
                set_flag(&unit, kFlagNumeric, true);
            } else if (unit.current == BidiClass::CS && before == after &&
                       (before == BidiClass::EN || before == BidiClass::AN)) {
                unit.current = before;
                set_flag(&unit, kFlagNumeric, true);
            }
        }
    }

    std::size_t position = 0U;
    while (position < sequence.size()) {
        if ((*units)[sequence[position]].current != BidiClass::ET) {
            ++position;
            continue;
        }
        const std::size_t start = position;
        while (position < sequence.size() &&
               (*units)[sequence[position]].current == BidiClass::ET) {
            ++position;
        }
        const BidiClass before = start == 0U
            ? BidiClass::ON
            : (*units)[sequence[start - 1U]].current;
        const BidiClass after = position == sequence.size()
            ? BidiClass::ON
            : (*units)[sequence[position]].current;
        if (before == BidiClass::EN || after == BidiClass::EN) {
            for (std::size_t item = start; item < position; ++item) {
                Unit& unit = (*units)[sequence[item]];
                unit.current = BidiClass::EN;
                set_flag(&unit, kFlagNumeric, true);
            }
        }
    }

    for (const std::uint32_t index : sequence) {
        Unit& unit = (*units)[index];
        if (unit.current == BidiClass::ES || unit.current == BidiClass::ET ||
            unit.current == BidiClass::CS) {
            unit.current = BidiClass::ON;
        }
    }

    last_strong = sos;
    for (const std::uint32_t index : sequence) {
        Unit& unit = (*units)[index];
        if (unit.current == BidiClass::L || unit.current == BidiClass::R) {
            last_strong = unit.current;
        } else if (unit.current == BidiClass::EN && last_strong == BidiClass::L) {
            unit.current = BidiClass::L;
            set_flag(&unit, kFlagNumeric, false);
        }
    }
}

bool brackets_match(std::uint32_t expected, std::uint32_t closing) noexcept {
    return expected == closing ||
           (expected == 0x232aU && closing == 0x3009U) ||
           (expected == 0x3009U && closing == 0x232aU);
}

void set_bracket_direction(
    std::pmr::vector<Unit>* units,
    const std::pmr::vector<std::uint32_t>& sequence,
    std::uint32_t opening_position,
    std::uint32_t closing_position,
    BidiClass direction) noexcept {
    const std::array<std::uint32_t, 2U> positions{
        opening_position,
        closing_position,
    };
    for (const std::uint32_t position : positions) {
        Unit& bracket = (*units)[sequence[position]];
        bracket.current = direction;
        std::size_t following = static_cast<std::size_t>(position) + 1U;
        while (following < sequence.size()) {
            Unit& unit = (*units)[sequence[following]];
            if (unit.original != BidiClass::NSM) {
                break;
            }
            unit.current = direction;
            ++following;
        }
    }
}

bool resolve_paired_brackets(
    std::pmr::vector<Unit>* units,
    const std::pmr::vector<std::uint32_t>& sequence,
    BidiClass sos,
    std::uint8_t embedding_level,
    std::pmr::vector<BracketPair>* pairs,
    BidiStats* stats,
    BidiError* error) noexcept {
    std::array<BracketStackEntry, kMaximumBracketDepth> stack{};
    std::size_t stack_size = 0U;
    pairs->clear();
    try {
        for (std::size_t position = 0U; position < sequence.size(); ++position) {
            const Unit& unit = (*units)[sequence[position]];
            if (!has_flag(unit, kFlagHasCodepoint) ||
                unit.current != BidiClass::ON) {
                continue;
            }
            const BidiBracketInfo bracket = bidi_bracket_of(unit.codepoint);
            if (bracket.type == BidiBracketType::Open) {
                if (stack_size == stack.size()) {
                    ++stats->bracket_stack_overflows;
                    break;
                }
                stack[stack_size++] = {
                    bracket.paired_codepoint,
                    static_cast<std::uint32_t>(position),
                };
            } else if (bracket.type == BidiBracketType::Close) {
                std::size_t depth = stack_size;
                while (depth > 0U) {
                    --depth;
                    if (brackets_match(
                            stack[depth].expected_closing,
                            unit.codepoint)) {
                        pairs->push_back({
                            stack[depth].sequence_position,
                            static_cast<std::uint32_t>(position),
                        });
                        stack_size = depth;
                        break;
                    }
                }
            }
        }
        std::sort(
            pairs->begin(),
            pairs->end(),
            [](const BracketPair& left, const BracketPair& right) {
                return left.opening_position < right.opening_position;
            });
    } catch (const std::bad_alloc&) {
        return fail(
            BidiErrorKind::OutputBudgetExceeded,
            0U,
            "bidi bracket pairing exceeded its resource budget",
            error);
    }

    const BidiClass embedding_direction = direction_from_level(embedding_level);
    const BidiClass opposite_direction = embedding_direction == BidiClass::L
        ? BidiClass::R
        : BidiClass::L;
    for (const BracketPair& pair : *pairs) {
        bool embedding_found = false;
        bool opposite_found = false;
        for (std::size_t position =
                 static_cast<std::size_t>(pair.opening_position) + 1U;
             position < pair.closing_position;
             ++position) {
            const BidiClass strong =
                strong_for_bracket((*units)[sequence[position]]);
            if (strong == embedding_direction) {
                embedding_found = true;
                break;
            }
            if (strong == opposite_direction) {
                opposite_found = true;
            }
        }
        if (embedding_found) {
            set_bracket_direction(
                units,
                sequence,
                pair.opening_position,
                pair.closing_position,
                embedding_direction);
            continue;
        }
        if (!opposite_found) {
            continue;
        }
        BidiClass prior = sos;
        for (std::size_t position = 0U;
             position < pair.opening_position;
             ++position) {
            const BidiClass strong =
                strong_for_bracket((*units)[sequence[position]]);
            if (strong == BidiClass::L || strong == BidiClass::R) {
                prior = strong;
            }
        }
        set_bracket_direction(
            units,
            sequence,
            pair.opening_position,
            pair.closing_position,
            prior == opposite_direction
                ? opposite_direction
                : embedding_direction);
    }
    stats->bracket_pairs += static_cast<std::uint64_t>(pairs->size());
    return true;
}

void resolve_neutral_types(
    std::pmr::vector<Unit>* units,
    const std::pmr::vector<std::uint32_t>& sequence,
    BidiClass sos,
    BidiClass eos,
    std::uint8_t embedding_level) noexcept {
    std::size_t position = 0U;
    while (position < sequence.size()) {
        if (!neutral_type((*units)[sequence[position]].current)) {
            ++position;
            continue;
        }
        const std::size_t start = position;
        while (position < sequence.size() &&
               neutral_type((*units)[sequence[position]].current)) {
            ++position;
        }
        BidiClass before = start == 0U
            ? sos
            : strong_for_neutral((*units)[sequence[start - 1U]]);
        BidiClass after = position == sequence.size()
            ? eos
            : strong_for_neutral((*units)[sequence[position]]);
        if (before != BidiClass::L && before != BidiClass::R) {
            before = sos;
        }
        if (after != BidiClass::L && after != BidiClass::R) {
            after = eos;
        }
        const BidiClass resolved = before == after
            ? before
            : direction_from_level(embedding_level);
        for (std::size_t item = start; item < position; ++item) {
            (*units)[sequence[item]].current = resolved;
        }
    }
}

bool process_sequence(
    std::pmr::vector<Unit>* units,
    const std::pmr::vector<std::uint32_t>& sequence,
    BidiClass sos,
    BidiClass eos,
    std::uint8_t embedding_level,
    std::pmr::vector<BracketPair>* pairs,
    BidiStats* stats,
    BidiError* error) noexcept {
    resolve_weak_types(units, sequence, sos);
    if (!resolve_paired_brackets(
            units,
            sequence,
            sos,
            embedding_level,
            pairs,
            stats,
            error)) {
        return false;
    }
    resolve_neutral_types(units, sequence, sos, eos, embedding_level);
    stats->maximum_sequence_units = std::max(
        stats->maximum_sequence_units,
        static_cast<std::uint64_t>(sequence.size()));
    ++stats->isolating_run_sequences;
    return true;
}

bool process_isolating_sequences(
    std::pmr::vector<Unit>* units,
    std::uint8_t paragraph_level,
    const std::pmr::vector<std::int32_t>& matching_pdi,
    const std::pmr::vector<std::uint32_t>& visible,
    std::pmr::vector<LevelRun>* runs,
    std::pmr::memory_resource* memory,
    BidiStats* stats,
    BidiError* error) noexcept {
    try {
        std::pmr::vector<std::uint8_t> assigned(memory);
        assigned.assign(runs->size(), 0U);
        std::pmr::vector<std::uint32_t> sequence(memory);
        sequence.reserve(std::min<std::size_t>(visible.size(), 256U));
        std::pmr::vector<BracketPair> pairs(memory);
        pairs.reserve(std::min<std::size_t>(visible.size() / 2U, 128U));

        const auto process_from = [&](std::size_t first_run) -> bool {
            sequence.clear();
            std::size_t run_index = first_run;
            std::size_t last_run = first_run;
            while (run_index < runs->size() && assigned[run_index] == 0U) {
                assigned[run_index] = 1U;
                const LevelRun& run = (*runs)[run_index];
                for (std::size_t position = run.visible_start;
                     position < run.visible_end;
                     ++position) {
                    sequence.push_back(visible[position]);
                }
                last_run = run_index;
                if (run.next < 0) {
                    break;
                }
                run_index = static_cast<std::size_t>(run.next);
            }
            if (sequence.empty()) {
                return true;
            }
            const LevelRun& first = (*runs)[first_run];
            const LevelRun& last = (*runs)[last_run];
            BidiClass eos = last.eor;
            const std::uint32_t final_index = sequence.back();
            if (is_isolate_initiator((*units)[final_index].original) &&
                matching_pdi[final_index] < 0) {
                eos = direction_from_level(
                    std::max(last.level, paragraph_level));
            }
            return process_sequence(
                units,
                sequence,
                first.sor,
                eos,
                first.level,
                &pairs,
                stats,
                error);
        };

        for (std::size_t run_index = 0U; run_index < runs->size(); ++run_index) {
            if (!(*runs)[run_index].incoming && assigned[run_index] == 0U &&
                !process_from(run_index)) {
                return false;
            }
        }
        for (std::size_t run_index = 0U; run_index < runs->size(); ++run_index) {
            if (assigned[run_index] == 0U && !process_from(run_index)) {
                return false;
            }
        }
        return true;
    } catch (const std::bad_alloc&) {
        return fail(
            BidiErrorKind::OutputBudgetExceeded,
            0U,
            "bidi isolating-run processing exceeded its resource budget",
            error);
    } catch (...) {
        return fail(
            BidiErrorKind::InternalInvariant,
            0U,
            "bidi isolating-run processing failed",
            error);
    }
}

void resolve_implicit_levels(
    std::pmr::vector<Unit>* units,
    BidiStats* stats) noexcept {
    for (Unit& unit : *units) {
        if (has_flag(unit, kFlagRemoved)) {
            continue;
        }
        if ((unit.level % 2U) == 0U) {
            if (unit.current == BidiClass::R) {
                unit.level = static_cast<std::uint8_t>(unit.level + 1U);
            } else if (unit.current == BidiClass::EN ||
                       unit.current == BidiClass::AN) {
                unit.level = static_cast<std::uint8_t>(unit.level + 2U);
            }
        } else if (unit.current == BidiClass::L ||
                   unit.current == BidiClass::EN ||
                   unit.current == BidiClass::AN) {
            unit.level = static_cast<std::uint8_t>(unit.level + 1U);
        }
        stats->maximum_resolved_level =
            std::max(stats->maximum_resolved_level, unit.level);
    }
}

bool l1_reset_candidate(BidiClass value) noexcept {
    return value == BidiClass::WS || is_isolate_initiator(value) ||
           value == BidiClass::PDI || value == BidiClass::RLE ||
           value == BidiClass::LRE || value == BidiClass::RLO ||
           value == BidiClass::LRO || value == BidiClass::PDF ||
           value == BidiClass::BN;
}

void reset_whitespace_levels(
    std::pmr::vector<Unit>* units,
    std::uint8_t paragraph_level) noexcept {
    for (std::size_t index = 0U; index < units->size(); ++index) {
        if ((*units)[index].original != BidiClass::B &&
            (*units)[index].original != BidiClass::S) {
            continue;
        }
        (*units)[index].level = paragraph_level;
        std::size_t preceding = index;
        while (preceding > 0U &&
               l1_reset_candidate((*units)[preceding - 1U].original)) {
            --preceding;
            (*units)[preceding].level = paragraph_level;
        }
    }
    std::size_t index = units->size();
    while (index > 0U && l1_reset_candidate((*units)[index - 1U].original)) {
        --index;
        (*units)[index].level = paragraph_level;
    }
}

bool write_outputs(
    const std::pmr::vector<Unit>& units,
    std::pmr::vector<std::uint8_t>* resolved_levels,
    std::pmr::vector<std::uint32_t>* visual_order,
    BidiError* error) noexcept {
    try {
        resolved_levels->resize(units.size());
        visual_order->clear();
        visual_order->reserve(units.size());
        std::uint8_t maximum_level = 0U;
        std::uint8_t minimum_odd = kBidiRemovedLevel;
        for (std::size_t index = 0U; index < units.size(); ++index) {
            const Unit& unit = units[index];
            if (has_flag(unit, kFlagRemoved)) {
                (*resolved_levels)[index] = kBidiRemovedLevel;
                continue;
            }
            (*resolved_levels)[index] = unit.level;
            visual_order->push_back(static_cast<std::uint32_t>(index));
            maximum_level = std::max(maximum_level, unit.level);
            if ((unit.level % 2U) == 1U) {
                minimum_odd = std::min(minimum_odd, unit.level);
            }
        }
        if (minimum_odd == kBidiRemovedLevel) {
            return true;
        }
        for (int level = static_cast<int>(maximum_level);
             level >= static_cast<int>(minimum_odd);
             --level) {
            std::size_t position = 0U;
            while (position < visual_order->size()) {
                if (units[(*visual_order)[position]].level < level) {
                    ++position;
                    continue;
                }
                const std::size_t start = position;
                while (position < visual_order->size() &&
                       units[(*visual_order)[position]].level >= level) {
                    ++position;
                }
                std::reverse(
                    visual_order->begin() + static_cast<std::ptrdiff_t>(start),
                    visual_order->begin() + static_cast<std::ptrdiff_t>(position));
            }
        }
        return true;
    } catch (const std::bad_alloc&) {
        resolved_levels->clear();
        visual_order->clear();
        return fail(
            BidiErrorKind::OutputBudgetExceeded,
            0U,
            "bidi output exceeded its resource budget",
            error);
    } catch (...) {
        resolved_levels->clear();
        visual_order->clear();
        return fail(
            BidiErrorKind::InternalInvariant,
            0U,
            "bidi output materialization failed",
            error);
    }
}

template <typename Provider>
bool resolve_impl(
    std::size_t input_size,
    Provider provider,
    ParagraphDirection direction,
    std::pmr::vector<std::uint8_t>* resolved_levels,
    std::pmr::vector<std::uint32_t>* visual_order,
    BidiStats* stats,
    BidiError* error) noexcept {
    if (resolved_levels == nullptr || visual_order == nullptr ||
        stats == nullptr || error == nullptr) {
        return false;
    }
    clear_error(error);
    resolved_levels->clear();
    visual_order->clear();
    *stats = {};
    stats->input_units = static_cast<std::uint64_t>(input_size);
    if (resolved_levels->get_allocator().resource() !=
        visual_order->get_allocator().resource()) {
        return fail(
            BidiErrorKind::InvalidInput,
            0U,
            "bidi level and visual-order outputs must share one memory resource",
            error);
    }
    if (input_size > static_cast<std::size_t>(
                         std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            BidiErrorKind::IndexOverflow,
            input_size,
            "bidi input exceeds 32-bit visual-order indexing",
            error);
    }
    if (direction != ParagraphDirection::Auto &&
        direction != ParagraphDirection::LeftToRight &&
        direction != ParagraphDirection::RightToLeft) {
        return fail(
            BidiErrorKind::InvalidInput,
            0U,
            "invalid paragraph direction",
            error);
    }

    std::pmr::memory_resource* memory =
        resolved_levels->get_allocator().resource();
    try {
        std::pmr::vector<Unit> units(memory);
        units.reserve(input_size);
        for (std::size_t index = 0U; index < input_size; ++index) {
            Unit unit = provider(index);
            if (!valid_bidi_class(unit.original)) {
                return fail(
                    BidiErrorKind::InvalidInput,
                    index,
                    "input contains an invalid Bidi_Class value",
                    error);
            }
            unit.current = unit.original;
            set_flag(
                &unit,
                kFlagNumeric,
                unit.original == BidiClass::EN || unit.original == BidiClass::AN);
            units.push_back(unit);
        }
        if (units.empty()) {
            return true;
        }

        std::uint8_t paragraph_level = 0U;
        {
            std::pmr::vector<std::int32_t> matching_pdi(memory);
            if (!match_isolates(
                    units,
                    &matching_pdi,
                    memory,
                    error)) {
                return false;
            }
            paragraph_level = direction == ParagraphDirection::Auto
                ? decide_paragraph_level(units, 0U, units.size(), matching_pdi)
                : static_cast<std::uint8_t>(direction);
            stats->paragraph_level = paragraph_level;
            apply_explicit_levels(
                &units,
                paragraph_level,
                matching_pdi,
                stats);
    
            std::pmr::vector<std::uint32_t> visible(memory);
            std::pmr::vector<LevelRun> runs(memory);
            std::pmr::vector<std::int32_t> index_to_run(memory);
            if (!build_level_runs(
                    units,
                    paragraph_level,
                    matching_pdi,
                    &visible,
                    &runs,
                    &index_to_run,
                    stats,
                    error)) {
                return false;
            }
            if (!runs.empty() &&
                !process_isolating_sequences(
                    &units,
                    paragraph_level,
                    matching_pdi,
                    visible,
                    &runs,
                    memory,
                    stats,
                    error)) {
                return false;
            }
        }
        resolve_implicit_levels(&units, stats);
        reset_whitespace_levels(&units, paragraph_level);
        return write_outputs(units, resolved_levels, visual_order, error);
    } catch (const std::bad_alloc&) {
        resolved_levels->clear();
        visual_order->clear();
        return fail(
            BidiErrorKind::OutputBudgetExceeded,
            0U,
            "bidi working set exceeded its resource budget",
            error);
    } catch (...) {
        resolved_levels->clear();
        visual_order->clear();
        return fail(
            BidiErrorKind::InternalInvariant,
            0U,
            "bidi resolution failed",
            error);
    }
}

} // namespace

const char* bidi_error_kind_name(BidiErrorKind kind) noexcept {
    switch (kind) {
        case BidiErrorKind::None:
            return "none";
        case BidiErrorKind::InvalidInput:
            return "invalid_input";
        case BidiErrorKind::IndexOverflow:
            return "index_overflow";
        case BidiErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
        case BidiErrorKind::InternalInvariant:
            return "internal_invariant";
    }
    return "invalid";
}

bool resolve_bidi_classes(
    std::span<const BidiClass> classes,
    ParagraphDirection direction,
    std::pmr::vector<std::uint8_t>* resolved_levels,
    std::pmr::vector<std::uint32_t>* visual_order,
    BidiStats* stats,
    BidiError* error) noexcept {
    return resolve_impl(
        classes.size(),
        [&](std::size_t index) noexcept {
            Unit unit;
            unit.original = classes[index];
            return unit;
        },
        direction,
        resolved_levels,
        visual_order,
        stats,
        error);
}

bool resolve_bidi_codepoints(
    std::span<const DecodedCodePoint> codepoints,
    ParagraphDirection direction,
    std::pmr::vector<std::uint8_t>* resolved_levels,
    std::pmr::vector<std::uint32_t>* visual_order,
    BidiStats* stats,
    BidiError* error) noexcept {
    for (std::size_t index = 0U; index < codepoints.size(); ++index) {
        const DecodedCodePoint& codepoint = codepoints[index];
        if (codepoint.value > 0x10ffffU ||
            (codepoint.value >= 0xd800U && codepoint.value <= 0xdfffU) ||
            codepoint.source_length == 0U || codepoint.source_length > 4U ||
            (index != 0U &&
             codepoint.source_start != codepoints[index - 1U].source_end())) {
            if (error != nullptr) {
                clear_error(error);
                fail(
                    BidiErrorKind::InvalidInput,
                    index,
                    "decoded bidi input is not a contiguous Unicode scalar stream",
                    error);
            }
            if (resolved_levels != nullptr) {
                resolved_levels->clear();
            }
            if (visual_order != nullptr) {
                visual_order->clear();
            }
            if (stats != nullptr) {
                *stats = {};
            }
            return false;
        }
    }
    return resolve_impl(
        codepoints.size(),
        [&](std::size_t index) noexcept {
            Unit unit;
            unit.codepoint = codepoints[index].value;
            unit.original = bidi_class_of(codepoints[index].value);
            set_flag(&unit, kFlagHasCodepoint, true);
            return unit;
        },
        direction,
        resolved_levels,
        visual_order,
        stats,
        error);
}

} // namespace zevryon::text
