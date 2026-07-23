#include "bidi_mirroring.hpp"

#include "unicode_bidi_mirroring.hpp"

#include <limits>
#include <new>

namespace zevryon::text {
namespace {

constexpr std::uint8_t kMaximumResolvedLevel = 126U;

void clear_error(BidiMirrorError* error) noexcept {
    if (error != nullptr) {
        error->kind = BidiMirrorErrorKind::None;
        error->visual_index = 0U;
        error->message.clear();
    }
}

bool fail(
    BidiMirrorErrorKind kind,
    std::size_t visual_index,
    const char* message,
    BidiMirrorError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->visual_index = visual_index;
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

bool validate_stage_contract(
    std::span<const DecodedCodePoint> codepoints,
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    const BidiVisualOrder& visual,
    BidiMirrorError* error) noexcept {
    const std::size_t active_count = topology.active_unit_indices.size();
    if (active_count > static_cast<std::size_t>(
                           std::numeric_limits<std::uint32_t>::max()) ||
        visual.line_levels.size() != active_count ||
        visual.visual_to_active.size() != active_count) {
        return fail(
            BidiMirrorErrorKind::InvalidInput,
            0U,
            "active and visual stream sizes are inconsistent",
            error);
    }

    for (std::size_t active = 0U; active < active_count; ++active) {
        const std::uint32_t unit_index = topology.active_unit_indices[active];
        if (unit_index >= units.size() ||
            (active != 0U &&
             topology.active_unit_indices[active - 1U] >= unit_index) ||
            (units[unit_index].flags & kBidiUnitRemovedByX9) != 0U ||
            units[unit_index].codepoint_index >= codepoints.size() ||
            visual.line_levels[active] > kMaximumResolvedLevel) {
            return fail(
                BidiMirrorErrorKind::InvalidInput,
                active,
                "active indices, codepoint links, or final levels are invalid",
                error);
        }
    }

    for (std::size_t visual_index = 0U; visual_index < active_count; ++visual_index) {
        if (visual.visual_to_active[visual_index] >= active_count) {
            return fail(
                BidiMirrorErrorKind::InvalidInput,
                visual_index,
                "visual order contains an out-of-range active index",
                error);
        }
    }
    return true;
}

BidiMirrorKind request_kind(const BidiMirroringInfo& info) noexcept {
    if (!info.has_character_mapping) {
        return BidiMirrorKind::MirroredGlyphOnly;
    }
    return info.best_fit
        ? BidiMirrorKind::BestFitCharacter
        : BidiMirrorKind::ExactCharacter;
}

void record_kind(BidiMirrorKind kind, BidiMirrorStats* stats) noexcept {
    switch (kind) {
        case BidiMirrorKind::ExactCharacter:
            ++stats->exact_character_requests;
            return;
        case BidiMirrorKind::BestFitCharacter:
            ++stats->best_fit_character_requests;
            return;
        case BidiMirrorKind::MirroredGlyphOnly:
            ++stats->glyph_only_requests;
            return;
    }
}

} // namespace

BidiMirrorRequests::BidiMirrorRequests(std::pmr::memory_resource* resource)
    : requests(resource) {}

std::pmr::memory_resource* BidiMirrorRequests::resource() const noexcept {
    return requests.get_allocator().resource();
}

void BidiMirrorRequests::release() noexcept {
    release_vector(&requests);
}

const char* bidi_mirror_error_kind_name(BidiMirrorErrorKind kind) noexcept {
    switch (kind) {
        case BidiMirrorErrorKind::None:
            return "none";
        case BidiMirrorErrorKind::InvalidInput:
            return "invalid_input";
        case BidiMirrorErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
    }
    return "invalid";
}

bool build_bidi_mirror_requests(
    std::span<const DecodedCodePoint> codepoints,
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    const BidiVisualOrder& visual,
    BidiMirrorRequests* output,
    BidiMirrorStats* stats,
    BidiMirrorError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    clear_error(error);
    output->release();
    *stats = {};

    if (!validate_stage_contract(codepoints, units, topology, visual, error)) {
        return false;
    }

    BidiMirrorStats working_stats;
    working_stats.input_codepoints = static_cast<std::uint64_t>(codepoints.size());
    working_stats.active_units =
        static_cast<std::uint64_t>(topology.active_unit_indices.size());
    working_stats.visual_units =
        static_cast<std::uint64_t>(visual.visual_to_active.size());

    std::size_t request_count = 0U;
    for (std::size_t visual_index = 0U;
         visual_index < visual.visual_to_active.size();
         ++visual_index) {
        const std::uint32_t active = visual.visual_to_active[visual_index];
        if ((visual.line_levels[active] & 1U) == 0U) {
            continue;
        }
        ++working_stats.odd_level_units;
        const std::uint32_t unit_index = topology.active_unit_indices[active];
        const std::uint32_t codepoint =
            codepoints[units[unit_index].codepoint_index].value;
        if (!bidi_mirroring_info(codepoint).mirrored) {
            continue;
        }
        ++working_stats.mirrored_property_hits;
        ++request_count;
    }

    try {
        BidiMirrorRequests working(output->resource());
        working.requests.reserve(request_count);
        for (std::size_t visual_index = 0U;
             visual_index < visual.visual_to_active.size();
             ++visual_index) {
            const std::uint32_t active = visual.visual_to_active[visual_index];
            if ((visual.line_levels[active] & 1U) == 0U) {
                continue;
            }
            const std::uint32_t unit_index = topology.active_unit_indices[active];
            const std::uint32_t codepoint =
                codepoints[units[unit_index].codepoint_index].value;
            const BidiMirroringInfo info = bidi_mirroring_info(codepoint);
            if (!info.mirrored) {
                continue;
            }
            const BidiMirrorKind kind = request_kind(info);
            working.requests.push_back(BidiMirrorRequest{
                static_cast<std::uint32_t>(visual_index),
                info.has_character_mapping ? info.mirror_codepoint : 0U,
                kind,
                0U,
                0U});
            record_kind(kind, &working_stats);
        }
        working_stats.output_requests =
            static_cast<std::uint64_t>(working.requests.size());
        output->requests.swap(working.requests);
        *stats = working_stats;
        return true;
    } catch (const std::bad_alloc&) {
        return fail(
            BidiMirrorErrorKind::OutputBudgetExceeded,
            0U,
            "L4 mirror requests exceeded their resource budget",
            error);
    } catch (...) {
        return fail(
            BidiMirrorErrorKind::OutputBudgetExceeded,
            0U,
            "L4 mirror request allocation failed",
            error);
    }
}

} // namespace zevryon::text
