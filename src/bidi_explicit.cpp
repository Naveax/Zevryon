#include "bidi_explicit.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>

namespace zevryon::text {
namespace {

constexpr std::uint8_t kMaximumExplicitLevel = 125U;
constexpr std::uint8_t kUnknownDirection = 0xffU;
constexpr std::size_t kMaximumStatusDepth = 126U;

struct StatusEntry {
    std::uint8_t level{0};
    BidiOverrideStatus override_status{BidiOverrideStatus::Neutral};
    bool isolate{false};
};

struct IsolateFrame {
    std::size_t initiator_index{0};
    std::uint8_t first_strong{kUnknownDirection};
};

void clear_error(BidiExplicitError* error) noexcept {
    if (error != nullptr) {
        error->kind = BidiExplicitErrorKind::None;
        error->codepoint_index = 0U;
        error->message.clear();
    }
}

bool fail(
    BidiExplicitErrorKind kind,
    std::size_t index,
    const char* message,
    BidiExplicitError* error) noexcept {
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

bool validate_input(
    std::span<const DecodedCodePoint> codepoints,
    BidiExplicitError* error) noexcept {
    for (std::size_t index = 0U; index < codepoints.size(); ++index) {
        const DecodedCodePoint& codepoint = codepoints[index];
        if (codepoint.source_length == 0U ||
            codepoint.source_length > 4U ||
            codepoint.value > 0x10ffffU ||
            (codepoint.value >= 0xd800U && codepoint.value <= 0xdfffU) ||
            (index != 0U &&
             codepoint.source_start != codepoints[index - 1U].source_end())) {
            return fail(
                BidiExplicitErrorKind::InvalidInput,
                index,
                "decoded input is not a contiguous Unicode scalar stream",
                error);
        }
    }
    return true;
}

std::uint8_t paragraph_level(
    std::span<const DecodedCodePoint> codepoints,
    BidiParagraphDirection direction) noexcept {
    if (direction == BidiParagraphDirection::Left) {
        return 0U;
    }
    if (direction == BidiParagraphDirection::Right) {
        return 1U;
    }

    std::size_t isolate_depth = 0U;
    for (const DecodedCodePoint& codepoint : codepoints) {
        const BidiClass value = bidi_class_of(codepoint.value);
        if (is_bidi_isolate_initiator(value)) {
            ++isolate_depth;
            continue;
        }
        if (value == BidiClass::PDI) {
            if (isolate_depth != 0U) {
                --isolate_depth;
            }
            continue;
        }
        if (isolate_depth != 0U) {
            continue;
        }
        if (is_bidi_strong_left(value)) {
            return 0U;
        }
        if (is_bidi_strong_right(value)) {
            return 1U;
        }
    }
    return 0U;
}

bool build_fsi_directions(
    std::span<const DecodedCodePoint> codepoints,
    std::pmr::vector<std::uint8_t>* directions,
    BidiExplicitError* error) noexcept {
    try {
        directions->assign(codepoints.size(), kUnknownDirection);
    } catch (const std::bad_alloc&) {
        return fail(
            BidiExplicitErrorKind::OutputBudgetExceeded,
            0U,
            "FSI direction state exceeded the bidi resource budget",
            error);
    } catch (...) {
        return fail(
            BidiExplicitErrorKind::OutputBudgetExceeded,
            0U,
            "FSI direction state allocation failed",
            error);
    }

    std::array<IsolateFrame, kMaximumStatusDepth> stack{};
    std::size_t stack_size = 0U;
    std::size_t overflow_depth = 0U;

    for (std::size_t index = 0U; index < codepoints.size(); ++index) {
        const BidiClass value = bidi_class_of(codepoints[index].value);
        if (is_bidi_isolate_initiator(value)) {
            if (overflow_depth != 0U) {
                ++overflow_depth;
            } else if (stack_size < stack.size()) {
                stack[stack_size++] = IsolateFrame{index, kUnknownDirection};
            } else {
                overflow_depth = 1U;
            }
            continue;
        }
        if (value == BidiClass::PDI) {
            if (overflow_depth != 0U) {
                --overflow_depth;
            } else if (stack_size != 0U) {
                const IsolateFrame frame = stack[--stack_size];
                if (bidi_class_of(codepoints[frame.initiator_index].value) ==
                    BidiClass::FSI) {
                    (*directions)[frame.initiator_index] = frame.first_strong;
                }
            }
            continue;
        }
        if (overflow_depth == 0U && stack_size != 0U &&
            stack[stack_size - 1U].first_strong == kUnknownDirection) {
            if (is_bidi_strong_left(value)) {
                stack[stack_size - 1U].first_strong = 0U;
            } else if (is_bidi_strong_right(value)) {
                stack[stack_size - 1U].first_strong = 1U;
            }
        }
    }

    while (stack_size != 0U) {
        const IsolateFrame frame = stack[--stack_size];
        if (bidi_class_of(codepoints[frame.initiator_index].value) ==
            BidiClass::FSI) {
            (*directions)[frame.initiator_index] = frame.first_strong;
        }
    }
    return true;
}

std::uint8_t next_even_level(std::uint8_t level) noexcept {
    const std::uint16_t incremented = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(level) + std::uint16_t{2U});
    const std::uint16_t value = static_cast<std::uint16_t>(
        incremented & std::uint16_t{0xfffeU});
    return value <= kMaximumExplicitLevel
        ? static_cast<std::uint8_t>(value)
        : kUnknownDirection;
}

std::uint8_t next_odd_level(std::uint8_t level) noexcept {
    const std::uint16_t value =
        (static_cast<std::uint16_t>(level) + 1U) | std::uint16_t{1U};
    return value <= kMaximumExplicitLevel
        ? static_cast<std::uint8_t>(value)
        : kUnknownDirection;
}

BidiClass apply_override(
    BidiClass original,
    BidiOverrideStatus override_status) noexcept {
    if (override_status == BidiOverrideStatus::Left) {
        return BidiClass::L;
    }
    if (override_status == BidiOverrideStatus::Right) {
        return BidiClass::R;
    }
    return original;
}

bool push_unit(
    const DecodedCodePoint& codepoint,
    std::size_t index,
    BidiClass original,
    BidiClass resolved,
    std::uint8_t level,
    std::uint8_t flags,
    std::pmr::vector<BidiExplicitUnit>* units,
    BidiExplicitError* error) noexcept {
    if (index > static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            BidiExplicitErrorKind::InvalidInput,
            index,
            "bidi codepoint index exceeds 32-bit storage",
            error);
    }
    try {
        units->push_back({
            codepoint.source_start,
            static_cast<std::uint32_t>(index),
            original,
            resolved,
            level,
            flags,
        });
    } catch (const std::bad_alloc&) {
        return fail(
            BidiExplicitErrorKind::OutputBudgetExceeded,
            index,
            "bidi explicit output exceeded its resource budget",
            error);
    } catch (...) {
        return fail(
            BidiExplicitErrorKind::OutputBudgetExceeded,
            index,
            "bidi explicit output allocation failed",
            error);
    }
    return true;
}

} // namespace

const char* bidi_explicit_error_kind_name(BidiExplicitErrorKind kind) noexcept {
    switch (kind) {
        case BidiExplicitErrorKind::None:
            return "none";
        case BidiExplicitErrorKind::InvalidInput:
            return "invalid_input";
        case BidiExplicitErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
    }
    return "invalid";
}

bool resolve_bidi_explicit(
    std::span<const DecodedCodePoint> codepoints,
    BidiParagraphDirection direction,
    std::pmr::vector<BidiExplicitUnit>* units,
    BidiExplicitStats* stats,
    BidiExplicitError* error) noexcept {
    if (units == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    clear_error(error);
    units->clear();
    *stats = {};
    stats->input_codepoints = static_cast<std::uint64_t>(codepoints.size());
    if (!validate_input(codepoints, error)) {
        return false;
    }
    if (codepoints.empty()) {
        return true;
    }

    const std::uint8_t base_level = paragraph_level(codepoints, direction);
    stats->paragraph_level = base_level;
    stats->maximum_level = base_level;

    std::pmr::vector<std::uint8_t> fsi_directions(
        units->get_allocator().resource());
    if (!build_fsi_directions(codepoints, &fsi_directions, error)) {
        return false;
    }
    try {
        units->reserve(codepoints.size());
    } catch (const std::bad_alloc&) {
        return fail(
            BidiExplicitErrorKind::OutputBudgetExceeded,
            0U,
            "bidi explicit output reserve exceeded its resource budget",
            error);
    } catch (...) {
        return fail(
            BidiExplicitErrorKind::OutputBudgetExceeded,
            0U,
            "bidi explicit output reserve failed",
            error);
    }

    std::array<StatusEntry, kMaximumStatusDepth> stack{};
    std::size_t stack_size = 1U;
    stack[0] = StatusEntry{base_level, BidiOverrideStatus::Neutral, false};
    std::size_t overflow_isolate_count = 0U;
    std::size_t overflow_embedding_count = 0U;
    std::size_t valid_isolate_count = 0U;

    for (std::size_t index = 0U; index < codepoints.size(); ++index) {
        const BidiClass original = bidi_class_of(codepoints[index].value);
        const StatusEntry current = stack[stack_size - 1U];
        std::uint8_t flags = 0U;
        BidiClass resolved = apply_override(original, current.override_status);

        if (original == BidiClass::LRE || original == BidiClass::RLE ||
            original == BidiClass::LRO || original == BidiClass::RLO) {
            flags |= kBidiUnitRemovedByX9;
            ++stats->explicit_controls;
            const bool right = original == BidiClass::RLE ||
                               original == BidiClass::RLO;
            const std::uint8_t new_level = right
                ? next_odd_level(current.level)
                : next_even_level(current.level);
            if (overflow_isolate_count == 0U &&
                overflow_embedding_count == 0U &&
                new_level != kUnknownDirection &&
                stack_size < stack.size()) {
                BidiOverrideStatus override_status = BidiOverrideStatus::Neutral;
                if (original == BidiClass::LRO) {
                    override_status = BidiOverrideStatus::Left;
                } else if (original == BidiClass::RLO) {
                    override_status = BidiOverrideStatus::Right;
                }
                stack[stack_size++] =
                    StatusEntry{new_level, override_status, false};
                stats->maximum_level = std::max(stats->maximum_level, new_level);
            } else if (overflow_isolate_count == 0U) {
                ++overflow_embedding_count;
                ++stats->overflow_embeddings;
            }
        } else if (is_bidi_isolate_initiator(original)) {
            flags |= kBidiUnitIsolateInitiator;
            ++stats->isolate_initiators;
            bool right = original == BidiClass::RLI;
            if (original == BidiClass::FSI) {
                ++stats->fsi_resolutions;
                const std::uint8_t detected = fsi_directions[index];
                right = detected == kUnknownDirection
                    ? base_level == 1U
                    : detected == 1U;
            }
            const std::uint8_t new_level = right
                ? next_odd_level(current.level)
                : next_even_level(current.level);
            if (overflow_isolate_count == 0U &&
                overflow_embedding_count == 0U &&
                new_level != kUnknownDirection &&
                stack_size < stack.size()) {
                stack[stack_size++] =
                    StatusEntry{new_level, BidiOverrideStatus::Neutral, true};
                ++valid_isolate_count;
                ++stats->valid_isolates;
                stats->maximum_level = std::max(stats->maximum_level, new_level);
            } else {
                ++overflow_isolate_count;
                ++stats->overflow_isolates;
            }
        } else if (original == BidiClass::PDF) {
            flags |= kBidiUnitRemovedByX9;
            ++stats->explicit_controls;
            if (overflow_isolate_count != 0U) {
                ++stats->unmatched_pdf;
            } else if (overflow_embedding_count != 0U) {
                --overflow_embedding_count;
            } else if (stack_size > 1U && !stack[stack_size - 1U].isolate) {
                --stack_size;
            } else {
                ++stats->unmatched_pdf;
            }
        } else if (original == BidiClass::PDI) {
            flags |= kBidiUnitPopDirectionalIsolate;
            if (overflow_isolate_count != 0U) {
                --overflow_isolate_count;
            } else if (valid_isolate_count == 0U) {
                ++stats->unmatched_pdi;
            } else {
                overflow_embedding_count = 0U;
                while (stack_size > 1U && !stack[stack_size - 1U].isolate) {
                    --stack_size;
                }
                if (stack_size > 1U) {
                    --stack_size;
                    --valid_isolate_count;
                }
            }
        } else if (original == BidiClass::BN) {
            flags |= kBidiUnitRemovedByX9;
            ++stats->explicit_controls;
        }

        if (!push_unit(
                codepoints[index],
                index,
                original,
                resolved,
                current.level,
                flags,
                units,
                error)) {
            return false;
        }
    }

    stats->output_units = static_cast<std::uint64_t>(units->size());
    return true;
}

} // namespace zevryon::text
