#include "native_damage_presentation.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <tuple>

namespace zevryon::text {
namespace {

constexpr std::uint64_t kFnvOffset = 1'469'598'103'934'665'603ULL;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;

void mix(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (std::uint32_t byte = 0U; byte < 8U; ++byte) {
        *hash ^= (value >> (byte * 8U)) & 0xffU;
        *hash *= kFnvPrime;
    }
}

bool checked_add_u64(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t* output) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    *output = left + right;
    return true;
}

bool checked_mul_u64(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t* output) noexcept {
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    *output = left * right;
    return true;
}

bool checked_add_i64_u64(
    std::int64_t start,
    std::uint64_t size,
    std::int64_t* limit) noexcept {
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    const std::int64_t signed_size = static_cast<std::int64_t>(size);
    if (start > std::numeric_limits<std::int64_t>::max() - signed_size) {
        return false;
    }
    *limit = start + signed_size;
    return true;
}

bool rect_valid(const NativeDamageRect& rect) noexcept {
    std::int64_t inline_limit = 0;
    std::int64_t block_limit = 0;
    return rect.inline_size != 0U &&
        rect.block_size != 0U &&
        checked_add_i64_u64(rect.inline_start, rect.inline_size, &inline_limit) &&
        checked_add_i64_u64(rect.block_start, rect.block_size, &block_limit);
}

bool rect_limits(
    const NativeDamageRect& rect,
    std::int64_t* inline_limit,
    std::int64_t* block_limit) noexcept {
    return checked_add_i64_u64(rect.inline_start, rect.inline_size, inline_limit) &&
        checked_add_i64_u64(rect.block_start, rect.block_size, block_limit);
}

bool intersect_rects(
    const NativeDamageRect& left,
    const NativeDamageRect& right,
    NativeDamageRect* output) noexcept {
    std::int64_t left_inline_limit = 0;
    std::int64_t left_block_limit = 0;
    std::int64_t right_inline_limit = 0;
    std::int64_t right_block_limit = 0;
    if (!rect_limits(left, &left_inline_limit, &left_block_limit) ||
        !rect_limits(right, &right_inline_limit, &right_block_limit)) {
        return false;
    }
    const std::int64_t inline_start = std::max(left.inline_start, right.inline_start);
    const std::int64_t block_start = std::max(left.block_start, right.block_start);
    const std::int64_t inline_limit = std::min(left_inline_limit, right_inline_limit);
    const std::int64_t block_limit = std::min(left_block_limit, right_block_limit);
    if (inline_start >= inline_limit || block_start >= block_limit) {
        *output = {};
        return true;
    }
    output->inline_start = inline_start;
    output->block_start = block_start;
    output->inline_size = static_cast<std::uint64_t>(inline_limit - inline_start);
    output->block_size = static_cast<std::uint64_t>(block_limit - block_start);
    return true;
}

bool union_rects(
    const NativeDamageRect& left,
    const NativeDamageRect& right,
    NativeDamageRect* output) noexcept {
    std::int64_t left_inline_limit = 0;
    std::int64_t left_block_limit = 0;
    std::int64_t right_inline_limit = 0;
    std::int64_t right_block_limit = 0;
    if (!rect_limits(left, &left_inline_limit, &left_block_limit) ||
        !rect_limits(right, &right_inline_limit, &right_block_limit)) {
        return false;
    }
    output->inline_start = std::min(left.inline_start, right.inline_start);
    output->block_start = std::min(left.block_start, right.block_start);
    const std::int64_t inline_limit = std::max(left_inline_limit, right_inline_limit);
    const std::int64_t block_limit = std::max(left_block_limit, right_block_limit);
    output->inline_size = static_cast<std::uint64_t>(inline_limit - output->inline_start);
    output->block_size = static_cast<std::uint64_t>(block_limit - output->block_start);
    return true;
}

bool rects_overlap_or_touch(
    const NativeDamageRect& left,
    const NativeDamageRect& right,
    bool touching) noexcept {
    std::int64_t left_inline_limit = 0;
    std::int64_t left_block_limit = 0;
    std::int64_t right_inline_limit = 0;
    std::int64_t right_block_limit = 0;
    if (!rect_limits(left, &left_inline_limit, &left_block_limit) ||
        !rect_limits(right, &right_inline_limit, &right_block_limit)) {
        return false;
    }
    if (touching) {
        return left.inline_start <= right_inline_limit &&
            right.inline_start <= left_inline_limit &&
            left.block_start <= right_block_limit &&
            right.block_start <= left_block_limit;
    }
    return left.inline_start < right_inline_limit &&
        right.inline_start < left_inline_limit &&
        left.block_start < right_block_limit &&
        right.block_start < left_block_limit;
}

bool rect_intersects(
    const NativeDamageRect& left,
    const NativeDamageRect& right) noexcept {
    return rects_overlap_or_touch(left, right, false);
}

bool rect_area(const NativeDamageRect& rect, std::uint64_t* area) noexcept {
    return checked_mul_u64(rect.inline_size, rect.block_size, area);
}

NativeDamageRect surface_rect(const GpuSurfaceDescriptor& surface) noexcept {
    return {
        0,
        0,
        static_cast<std::uint64_t>(surface.width),
        static_cast<std::uint64_t>(surface.height)};
}

bool clip_to_surface(
    const NativeDamageRect& input,
    const GpuSurfaceDescriptor& surface,
    NativeDamageRect* output) noexcept {
    return intersect_rects(input, surface_rect(surface), output);
}

bool clip_to_paint_clip(
    const NativeDamageRect& input,
    const TextPaintClipRect& clip,
    NativeDamageRect* output) noexcept {
    const NativeDamageRect clip_rect{
        clip.viewport_inline_start,
        clip.viewport_block_start,
        clip.inline_size,
        clip.block_size};
    return intersect_rects(input, clip_rect, output);
}

void set_build_error(
    NativeCommandBuildError* error,
    NativeCommandBuildErrorKind kind,
    std::string message,
    std::size_t command_index = 0U,
    std::size_t draw_index = 0U,
    std::size_t damage_index = 0U) noexcept {
    if (error == nullptr) {
        return;
    }
    try {
        error->kind = kind;
        error->command_index = command_index;
        error->draw_index = draw_index;
        error->damage_index = damage_index;
        error->message = std::move(message);
    } catch (...) {
        error->kind = kind;
        error->message.clear();
    }
}

void set_present_error(
    NativePresentError* error,
    NativePresentErrorKind kind,
    std::string message,
    const NativeGpuApiError* backend = nullptr) noexcept {
    if (error == nullptr) {
        return;
    }
    try {
        error->kind = kind;
        error->message = std::move(message);
        if (backend != nullptr) {
            error->backend_error = *backend;
        }
    } catch (...) {
        error->kind = kind;
        error->message.clear();
    }
}

bool add_damage_rect(
    std::pmr::vector<NativeDamageRect>* rects,
    NativeDamageRect rect,
    const GpuSurfaceDescriptor& surface,
    const NativeDamagePolicy& policy,
    NativeCommandBuildStats* stats,
    NativeCommandBuildError* error,
    std::size_t damage_index) {
    NativeDamageRect clipped;
    if (!clip_to_surface(rect, surface, &clipped)) {
        set_build_error(
            error,
            NativeCommandBuildErrorKind::ArithmeticOverflow,
            "damage clipping overflow",
            0U,
            0U,
            damage_index);
        return false;
    }
    if (clipped.inline_size == 0U || clipped.block_size == 0U) {
        return true;
    }
    const bool touching =
        (policy.flags & kNativeDamageMergeTouching) != 0U;
    for (;;) {
        bool merged = false;
        for (std::size_t index = 0U; index < rects->size(); ++index) {
            if (!rects_overlap_or_touch((*rects)[index], clipped, touching)) {
                continue;
            }
            NativeDamageRect combined;
            if (!union_rects((*rects)[index], clipped, &combined)) {
                set_build_error(
                    error,
                    NativeCommandBuildErrorKind::ArithmeticOverflow,
                    "damage union overflow",
                    0U,
                    0U,
                    damage_index);
                return false;
            }
            clipped = combined;
            rects->erase(rects->begin() + static_cast<std::ptrdiff_t>(index));
            if (stats != nullptr) {
                ++stats->merged_damage_rects;
            }
            merged = true;
            break;
        }
        if (!merged) {
            break;
        }
    }
    if (policy.maximum_rects != 0U && rects->size() >= policy.maximum_rects) {
        if ((policy.flags & kNativeDamageCollapseOnOverflow) == 0U) {
            set_build_error(
                error,
                NativeCommandBuildErrorKind::DamageLimitExceeded,
                "damage rectangle limit exceeded",
                0U,
                0U,
                damage_index);
            return false;
        }
        NativeDamageRect collapsed = clipped;
        for (const NativeDamageRect& existing : *rects) {
            NativeDamageRect combined;
            if (!union_rects(collapsed, existing, &combined)) {
                set_build_error(
                    error,
                    NativeCommandBuildErrorKind::ArithmeticOverflow,
                    "damage collapse overflow",
                    0U,
                    0U,
                    damage_index);
                return false;
            }
            collapsed = combined;
        }
        rects->clear();
        rects->push_back(collapsed);
        return true;
    }
    rects->push_back(clipped);
    return true;
}

bool footprint_for_command(
    const GpuFrameSubmission& frame,
    std::span<const GlyphAtlasDrawInstance> draw_instances,
    std::size_t command_index,
    NativeCommandFootprint* output,
    NativeCommandBuildError* error) noexcept {
    const GpuFrameCommandRecord& command = frame.commands[command_index];
    if (command.clip_index >= frame.clips.size()) {
        set_build_error(
            error,
            NativeCommandBuildErrorKind::FrameTopologyViolation,
            "command clip index is out of range",
            command_index);
        return false;
    }
    NativeDamageRect bounds;
    std::uint64_t hash = kFnvOffset;
    mix(&hash, static_cast<std::uint64_t>(command.kind));
    mix(&hash, command.payload_index);
    mix(&hash, command.clip_index);
    mix(&hash, command.flags);
    if (command.kind == GpuFrameCommandKind::FillRect) {
        if (command.payload_index >= frame.fill_rects.size()) {
            set_build_error(
                error,
                NativeCommandBuildErrorKind::FrameTopologyViolation,
                "fill-rect payload index is out of range",
                command_index);
            return false;
        }
        const TextPaintFillRect& fill = frame.fill_rects[command.payload_index];
        bounds = {
            fill.viewport_inline_start,
            fill.viewport_block_start,
            fill.inline_size,
            fill.block_size};
        mix(&hash, static_cast<std::uint64_t>(fill.viewport_inline_start));
        mix(&hash, static_cast<std::uint64_t>(fill.viewport_block_start));
        mix(&hash, fill.inline_size);
        mix(&hash, fill.block_size);
        mix(&hash, fill.style_id);
        mix(&hash, fill.source_line_index);
        mix(&hash, fill.source_fragment_index);
        mix(&hash, fill.flags);
    } else if (command.kind == GpuFrameCommandKind::GlyphBatch) {
        if (command.payload_index >= frame.glyph_batches.size()) {
            set_build_error(
                error,
                NativeCommandBuildErrorKind::FrameTopologyViolation,
                "glyph-batch payload index is out of range",
                command_index);
            return false;
        }
        const GpuFrameGlyphBatch& batch = frame.glyph_batches[command.payload_index];
        const std::uint64_t first = batch.first_instance;
        std::uint64_t limit = 0U;
        if (!checked_add_u64(first, batch.instance_count, &limit) ||
            limit > draw_instances.size()) {
            set_build_error(
                error,
                NativeCommandBuildErrorKind::DrawTopologyViolation,
                "glyph instance range is out of bounds",
                command_index,
                batch.first_instance);
            return false;
        }
        mix(&hash, batch.page_generation);
        mix(&hash, batch.page_index);
        mix(&hash, batch.first_instance);
        mix(&hash, batch.instance_count);
        mix(&hash, batch.style_id);
        mix(&hash, batch.clip_index);
        mix(&hash, batch.page_reference_index);
        mix(&hash, batch.flags);
        bool has_bounds = false;
        for (std::uint64_t instance_index = first;
             instance_index < limit;
             ++instance_index) {
            const GlyphAtlasDrawInstance& instance =
                draw_instances[static_cast<std::size_t>(instance_index)];
            mix(&hash, static_cast<std::uint64_t>(instance.viewport_inline_start));
            mix(&hash, static_cast<std::uint64_t>(instance.viewport_block_start));
            mix(&hash, instance.atlas_generation_id);
            mix(&hash, instance.page_generation);
            mix(&hash, instance.page_index);
            mix(&hash, instance.atlas_x);
            mix(&hash, instance.atlas_y);
            mix(&hash, instance.width);
            mix(&hash, instance.height);
            mix(&hash, instance.style_id);
            mix(&hash, instance.clip_index);
            mix(&hash, instance.working_set_key_index);
            if (instance.width == 0U || instance.height == 0U) {
                continue;
            }
            const NativeDamageRect instance_rect{
                instance.viewport_inline_start,
                instance.viewport_block_start,
                instance.width,
                instance.height};
            if (!rect_valid(instance_rect)) {
                set_build_error(
                    error,
                    NativeCommandBuildErrorKind::ArithmeticOverflow,
                    "glyph instance bounds overflow",
                    command_index,
                    static_cast<std::size_t>(instance_index));
                return false;
            }
            if (!has_bounds) {
                bounds = instance_rect;
                has_bounds = true;
            } else {
                NativeDamageRect combined;
                if (!union_rects(bounds, instance_rect, &combined)) {
                    set_build_error(
                        error,
                        NativeCommandBuildErrorKind::ArithmeticOverflow,
                        "glyph batch bounds overflow",
                        command_index,
                        static_cast<std::size_t>(instance_index));
                    return false;
                }
                bounds = combined;
            }
        }
        if (!has_bounds) {
            bounds = {};
        }
    } else {
        set_build_error(
            error,
            NativeCommandBuildErrorKind::FrameTopologyViolation,
            "unknown frame command kind",
            command_index);
        return false;
    }

    if (bounds.inline_size != 0U && bounds.block_size != 0U) {
        if (!rect_valid(bounds)) {
            set_build_error(
                error,
                NativeCommandBuildErrorKind::ArithmeticOverflow,
                "command bounds overflow",
                command_index);
            return false;
        }
        NativeDamageRect clipped_to_clip;
        if (!clip_to_paint_clip(bounds, frame.clips[command.clip_index], &clipped_to_clip)) {
            set_build_error(
                error,
                NativeCommandBuildErrorKind::ArithmeticOverflow,
                "paint clip overflow",
                command_index);
            return false;
        }
        NativeDamageRect clipped_to_surface;
        if (!clip_to_surface(clipped_to_clip, frame.surface, &clipped_to_surface)) {
            set_build_error(
                error,
                NativeCommandBuildErrorKind::ArithmeticOverflow,
                "surface clip overflow",
                command_index);
            return false;
        }
        bounds = clipped_to_surface;
    }
    output->bounds = bounds;
    output->checksum = hash;
    output->source_command_index = static_cast<std::uint32_t>(command_index);
    output->source_kind = command.kind;
    output->flags = bounds.inline_size == 0U || bounds.block_size == 0U
        ? kNativeCommandFootprintEmpty
        : 0U;
    return true;
}

std::size_t count_in_flight(
    const std::pmr::vector<NativeInFlightFrameRecord>& frames) noexcept {
    return static_cast<std::size_t>(std::count_if(
        frames.begin(),
        frames.end(),
        [](const NativeInFlightFrameRecord& frame) {
            return frame.occupied != 0U;
        }));
}

} // namespace

NativeCommandBuffer::NativeCommandBuffer(std::pmr::memory_resource* resource)
    : damage_rects(resource), footprints(resource), commands(resource) {}

std::pmr::memory_resource* NativeCommandBuffer::resource() const noexcept {
    return commands.get_allocator().resource();
}

void NativeCommandBuffer::release() noexcept {
    damage_rects.clear();
    footprints.clear();
    commands.clear();
    surface = {};
    frame_id = 0U;
    command_generation = 0U;
    source_frame_checksum = 0U;
    command_checksum = 0U;
    full_redraw = 0U;
}

const char* native_command_build_error_kind_name(
    NativeCommandBuildErrorKind kind) noexcept {
    switch (kind) {
    case NativeCommandBuildErrorKind::None: return "none";
    case NativeCommandBuildErrorKind::InvalidInput: return "invalid-input";
    case NativeCommandBuildErrorKind::FrameTopologyViolation: return "frame-topology-violation";
    case NativeCommandBuildErrorKind::DrawTopologyViolation: return "draw-topology-violation";
    case NativeCommandBuildErrorKind::InvalidDamageRect: return "invalid-damage-rect";
    case NativeCommandBuildErrorKind::ArithmeticOverflow: return "arithmetic-overflow";
    case NativeCommandBuildErrorKind::DamageLimitExceeded: return "damage-limit-exceeded";
    case NativeCommandBuildErrorKind::CommandLimitExceeded: return "command-limit-exceeded";
    case NativeCommandBuildErrorKind::OutputBudgetExceeded: return "output-budget-exceeded";
    case NativeCommandBuildErrorKind::AggregateOverflow: return "aggregate-overflow";
    }
    return "unknown";
}

bool build_native_command_buffer(
    const NativeCommandBuildRequest& request,
    NativeCommandBuffer* output,
    NativeCommandBuildStats* stats,
    NativeCommandBuildError* error) noexcept {
    if (output == nullptr) {
        set_build_error(error, NativeCommandBuildErrorKind::InvalidInput, "output is null");
        return false;
    }
    output->release();
    if (stats != nullptr) {
        *stats = {};
    }
    if (error != nullptr) {
        *error = {};
    }
    if (request.frame == nullptr ||
        request.command_generation == 0U ||
        request.frame->surface.surface_id == 0U ||
        request.frame->surface.generation_id == 0U ||
        request.frame->surface.width == 0U ||
        request.frame->surface.height == 0U ||
        request.policy.maximum_rects == 0U ||
        request.policy.maximum_commands == 0U ||
        request.policy.full_redraw_threshold_permille > 1'000U ||
        request.previous == output) {
        set_build_error(error, NativeCommandBuildErrorKind::InvalidInput, "invalid command build request");
        return false;
    }

    try {
        NativeCommandBuffer staged(output->resource());
        staged.surface = request.frame->surface;
        staged.frame_id = request.frame->frame_id;
        staged.command_generation = request.command_generation;
        staged.footprints.reserve(request.frame->commands.size());
        for (std::size_t command_index = 0U;
             command_index < request.frame->commands.size();
             ++command_index) {
            NativeCommandFootprint footprint;
            if (!footprint_for_command(
                    *request.frame,
                    request.draw_instances,
                    command_index,
                    &footprint,
                    error)) {
                return false;
            }
            staged.footprints.push_back(footprint);
        }
        if (stats != nullptr) {
            stats->input_commands = request.frame->commands.size();
            stats->input_draw_instances = request.draw_instances.size();
            stats->output_footprints = staged.footprints.size();
        }

        std::uint64_t source_checksum = kFnvOffset;
        for (const NativeCommandFootprint& footprint : staged.footprints) {
            mix(&source_checksum, footprint.checksum);
            mix(&source_checksum, static_cast<std::uint64_t>(footprint.bounds.inline_start));
            mix(&source_checksum, static_cast<std::uint64_t>(footprint.bounds.block_start));
            mix(&source_checksum, footprint.bounds.inline_size);
            mix(&source_checksum, footprint.bounds.block_size);
        }
        staged.source_frame_checksum = source_checksum;

        const bool previous_compatible = request.previous != nullptr &&
            request.previous->surface == request.frame->surface &&
            request.previous->command_generation + 1U == request.command_generation;
        const bool force_full =
            (request.policy.flags & kNativeDamageForceFullRedraw) != 0U ||
            !previous_compatible;
        if (force_full) {
            if (!add_damage_rect(
                    &staged.damage_rects,
                    surface_rect(request.frame->surface),
                    request.frame->surface,
                    request.policy,
                    stats,
                    error,
                    0U)) {
                return false;
            }
            staged.full_redraw = 1U;
        } else {
            const std::size_t shared = std::min(
                staged.footprints.size(),
                request.previous->footprints.size());
            for (std::size_t index = 0U; index < shared; ++index) {
                const NativeCommandFootprint& current = staged.footprints[index];
                const NativeCommandFootprint& previous = request.previous->footprints[index];
                if (current == previous) {
                    continue;
                }
                if ((previous.flags & kNativeCommandFootprintEmpty) == 0U &&
                    !add_damage_rect(
                        &staged.damage_rects,
                        previous.bounds,
                        request.frame->surface,
                        request.policy,
                        stats,
                        error,
                        index)) {
                    return false;
                }
                if ((current.flags & kNativeCommandFootprintEmpty) == 0U &&
                    !add_damage_rect(
                        &staged.damage_rects,
                        current.bounds,
                        request.frame->surface,
                        request.policy,
                        stats,
                        error,
                        index)) {
                    return false;
                }
                if (stats != nullptr) {
                    ++stats->changed_commands;
                }
            }
            for (std::size_t index = shared;
                 index < staged.footprints.size();
                 ++index) {
                if ((staged.footprints[index].flags & kNativeCommandFootprintEmpty) == 0U &&
                    !add_damage_rect(
                        &staged.damage_rects,
                        staged.footprints[index].bounds,
                        request.frame->surface,
                        request.policy,
                        stats,
                        error,
                        index)) {
                    return false;
                }
                if (stats != nullptr) {
                    ++stats->changed_commands;
                }
            }
            for (std::size_t index = shared;
                 index < request.previous->footprints.size();
                 ++index) {
                if ((request.previous->footprints[index].flags & kNativeCommandFootprintEmpty) == 0U &&
                    !add_damage_rect(
                        &staged.damage_rects,
                        request.previous->footprints[index].bounds,
                        request.frame->surface,
                        request.policy,
                        stats,
                        error,
                        index)) {
                    return false;
                }
                if (stats != nullptr) {
                    ++stats->removed_commands;
                }
            }
        }

        for (std::size_t index = 0U; index < request.invalidations.size(); ++index) {
            if (!rect_valid(request.invalidations[index])) {
                set_build_error(
                    error,
                    NativeCommandBuildErrorKind::InvalidDamageRect,
                    "invalid explicit damage rectangle",
                    0U,
                    0U,
                    index);
                return false;
            }
            if (!add_damage_rect(
                    &staged.damage_rects,
                    request.invalidations[index],
                    request.frame->surface,
                    request.policy,
                    stats,
                    error,
                    index)) {
                return false;
            }
            if (stats != nullptr) {
                ++stats->explicit_invalidations;
            }
        }

        std::uint64_t full_area = 0U;
        if (!checked_mul_u64(
                request.frame->surface.width,
                request.frame->surface.height,
                &full_area)) {
            set_build_error(error, NativeCommandBuildErrorKind::ArithmeticOverflow, "surface area overflow");
            return false;
        }
        std::uint64_t total_area = 0U;
        for (const NativeDamageRect& rect : staged.damage_rects) {
            std::uint64_t area = 0U;
            if (!rect_area(rect, &area) || !checked_add_u64(total_area, area, &total_area)) {
                set_build_error(error, NativeCommandBuildErrorKind::ArithmeticOverflow, "damage area overflow");
                return false;
            }
        }
        if (request.policy.maximum_total_area != 0U &&
            total_area > request.policy.maximum_total_area) {
            set_build_error(error, NativeCommandBuildErrorKind::DamageLimitExceeded, "damage area limit exceeded");
            return false;
        }
        if (request.policy.full_redraw_threshold_permille != 0U &&
            full_area != 0U &&
            total_area * 1'000U >=
                full_area * request.policy.full_redraw_threshold_permille) {
            staged.damage_rects.clear();
            staged.damage_rects.push_back(surface_rect(request.frame->surface));
            staged.full_redraw = 1U;
            total_area = full_area;
        }

        if (!staged.damage_rects.empty()) {
            staged.commands.push_back({NativeCommandKind::BeginRenderPass, 0U, 0U, 0U});
            std::vector<std::uint32_t> seen(staged.footprints.size(), 0U);
            for (std::size_t damage_index = 0U;
                 damage_index < staged.damage_rects.size();
                 ++damage_index) {
                staged.commands.push_back({
                    NativeCommandKind::SetScissor,
                    static_cast<std::uint32_t>(damage_index),
                    static_cast<std::uint32_t>(damage_index),
                    staged.full_redraw == 0U ? kNativeCommandPartialDamage : 0U});
                for (std::size_t command_index = 0U;
                     command_index < staged.footprints.size();
                     ++command_index) {
                    const NativeCommandFootprint& footprint = staged.footprints[command_index];
                    if ((footprint.flags & kNativeCommandFootprintEmpty) != 0U ||
                        !rect_intersects(footprint.bounds, staged.damage_rects[damage_index])) {
                        continue;
                    }
                    const GpuFrameCommandRecord& source = request.frame->commands[command_index];
                    NativeCommandRecord native;
                    native.kind = source.kind == GpuFrameCommandKind::FillRect
                        ? NativeCommandKind::FillRect
                        : NativeCommandKind::GlyphBatch;
                    native.payload_index = source.payload_index;
                    native.scissor_index = static_cast<std::uint32_t>(damage_index);
                    native.flags = staged.full_redraw == 0U
                        ? kNativeCommandPartialDamage
                        : 0U;
                    if (seen[command_index] != 0U) {
                        native.flags |= kNativeCommandDuplicatedAcrossDamage;
                        if (stats != nullptr) {
                            ++stats->duplicated_commands;
                        }
                    }
                    ++seen[command_index];
                    staged.commands.push_back(native);
                }
            }
            staged.commands.push_back({NativeCommandKind::EndRenderPass, 0U, 0U, 0U});
            if (stats != nullptr) {
                stats->culled_commands = static_cast<std::uint64_t>(std::count(
                    seen.begin(), seen.end(), 0U));
            }
        }

        if (staged.commands.size() > request.policy.maximum_commands) {
            set_build_error(error, NativeCommandBuildErrorKind::CommandLimitExceeded, "native command limit exceeded");
            return false;
        }
        std::uint64_t command_checksum = kFnvOffset;
        mix(&command_checksum, staged.surface.surface_id);
        mix(&command_checksum, staged.surface.generation_id);
        mix(&command_checksum, staged.frame_id);
        mix(&command_checksum, staged.command_generation);
        for (const NativeDamageRect& rect : staged.damage_rects) {
            mix(&command_checksum, static_cast<std::uint64_t>(rect.inline_start));
            mix(&command_checksum, static_cast<std::uint64_t>(rect.block_start));
            mix(&command_checksum, rect.inline_size);
            mix(&command_checksum, rect.block_size);
        }
        for (const NativeCommandRecord& command : staged.commands) {
            mix(&command_checksum, static_cast<std::uint64_t>(command.kind));
            mix(&command_checksum, command.payload_index);
            mix(&command_checksum, command.scissor_index);
            mix(&command_checksum, command.flags);
        }
        staged.command_checksum = command_checksum;

        output->surface = staged.surface;
        output->frame_id = staged.frame_id;
        output->command_generation = staged.command_generation;
        output->source_frame_checksum = staged.source_frame_checksum;
        output->command_checksum = staged.command_checksum;
        output->full_redraw = staged.full_redraw;
        output->damage_rects.swap(staged.damage_rects);
        output->footprints.swap(staged.footprints);
        output->commands.swap(staged.commands);
        if (stats != nullptr) {
            stats->output_damage_rects = output->damage_rects.size();
            stats->output_commands = output->commands.size();
            stats->total_damage_area = total_area;
            stats->full_surface_area = full_area;
            stats->full_redraw = output->full_redraw;
        }
        return true;
    } catch (const std::bad_alloc&) {
        output->release();
        set_build_error(error, NativeCommandBuildErrorKind::OutputBudgetExceeded, "native command output budget exceeded");
        return false;
    } catch (...) {
        output->release();
        set_build_error(error, NativeCommandBuildErrorKind::AggregateOverflow, "native command aggregation failed");
        return false;
    }
}

bool native_command_buffer_is_current(
    const GpuFrameSubmission& frame,
    const NativeCommandBuffer& commands) noexcept {
    return commands.command_generation != 0U &&
        commands.frame_id == frame.frame_id &&
        commands.surface == frame.surface &&
        commands.footprints.size() == frame.commands.size();
}

NativeGpuApiKind ReferenceNativeGpuCommandApi::kind() const noexcept {
    return NativeGpuApiKind::ReferenceCpu;
}

bool ReferenceNativeGpuCommandApi::configure_surface(
    const GpuSurfaceDescriptor& surface,
    std::uint32_t image_count,
    std::uint64_t device_generation,
    NativeGpuApiError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
    if (surface.surface_id == 0U || surface.generation_id == 0U ||
        surface.width == 0U || surface.height == 0U ||
        image_count == 0U || device_generation == 0U) {
        if (error != nullptr) {
            error->kind = NativeGpuApiErrorKind::InvalidInput;
            error->message = "invalid reference surface configuration";
        }
        return false;
    }
    surface_ = surface;
    image_count_ = image_count;
    device_generation_ = device_generation;
    next_image_index_ = 0U;
    ++next_image_generation_;
    return true;
}

bool ReferenceNativeGpuCommandApi::acquire_next_image(
    const GpuSurfaceDescriptor& surface,
    NativePresentMode,
    std::uint64_t,
    NativeSwapchainImageHandle* image,
    NativeAcquireStatus* status,
    NativeGpuApiError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
    if (image == nullptr || status == nullptr || surface != surface_ || image_count_ == 0U) {
        if (error != nullptr) {
            error->kind = NativeGpuApiErrorKind::InvalidInput;
            error->message = "reference acquire input mismatch";
        }
        return false;
    }
    *status = next_acquire_status_;
    next_acquire_status_ = NativeAcquireStatus::Acquired;
    if (*status != NativeAcquireStatus::Acquired) {
        *image = {};
        return true;
    }
    image->device_generation = device_generation_;
    image->surface_id = surface_.surface_id;
    image->surface_generation = surface_.generation_id;
    image->image_generation = next_image_generation_;
    image->image_index = next_image_index_;
    image->flags = 1U;
    next_image_index_ = (next_image_index_ + 1U) % image_count_;
    return true;
}

bool ReferenceNativeGpuCommandApi::encode_submit_present(
    const NativeSwapchainImageHandle& image,
    const NativeCommandBuffer& commands,
    const GpuFrameSubmission& frame,
    std::span<const GlyphAtlasDrawInstance> draw_instances,
    std::uint64_t ticket_id,
    std::uint64_t wait_fence_value,
    std::uint64_t* signal_fence_value,
    std::uint64_t* encoded_checksum,
    NativePresentStatus* status,
    NativeGpuApiError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
    if (signal_fence_value == nullptr || encoded_checksum == nullptr || status == nullptr ||
        image.device_generation != device_generation_ ||
        image.surface_id != surface_.surface_id ||
        image.surface_generation != surface_.generation_id ||
        !native_command_buffer_is_current(frame, commands) ||
        ticket_id == 0U) {
        if (error != nullptr) {
            error->kind = NativeGpuApiErrorKind::InvalidInput;
            error->message = "reference encode input mismatch";
        }
        return false;
    }
    *status = next_present_status_;
    next_present_status_ = NativePresentStatus::Presented;
    if (*status != NativePresentStatus::Presented) {
        *signal_fence_value = 0U;
        *encoded_checksum = 0U;
        return true;
    }
    if (next_fence_value_ <= wait_fence_value) {
        next_fence_value_ = wait_fence_value + 1U;
    }
    if (next_fence_value_ == 0U) {
        if (error != nullptr) {
            error->kind = NativeGpuApiErrorKind::FenceOverflow;
            error->message = "reference fence overflow";
        }
        return false;
    }
    std::uint64_t hash = kFnvOffset;
    mix(&hash, image.device_generation);
    mix(&hash, image.surface_id);
    mix(&hash, image.surface_generation);
    mix(&hash, image.image_generation);
    mix(&hash, image.image_index);
    mix(&hash, ticket_id);
    mix(&hash, wait_fence_value);
    mix(&hash, commands.command_checksum);
    for (const NativeCommandRecord& command : commands.commands) {
        mix(&hash, static_cast<std::uint64_t>(command.kind));
        mix(&hash, command.payload_index);
        mix(&hash, command.scissor_index);
        mix(&hash, command.flags);
        if (command.kind == NativeCommandKind::GlyphBatch) {
            if (command.payload_index >= frame.glyph_batches.size()) {
                if (error != nullptr) {
                    error->kind = NativeGpuApiErrorKind::EncodeFailed;
                    error->message = "glyph payload out of range during encode";
                }
                return false;
            }
            const GpuFrameGlyphBatch& batch = frame.glyph_batches[command.payload_index];
            const std::uint64_t limit = static_cast<std::uint64_t>(batch.first_instance) +
                batch.instance_count;
            if (limit > draw_instances.size()) {
                if (error != nullptr) {
                    error->kind = NativeGpuApiErrorKind::EncodeFailed;
                    error->message = "glyph instance range out of range during encode";
                }
                return false;
            }
            mix(&hash, batch.page_generation);
            mix(&hash, batch.page_index);
            mix(&hash, batch.first_instance);
            mix(&hash, batch.instance_count);
        }
    }
    *signal_fence_value = next_fence_value_++;
    *encoded_checksum = hash;
    return true;
}

void ReferenceNativeGpuCommandApi::set_next_acquire_status(
    NativeAcquireStatus status) noexcept {
    next_acquire_status_ = status;
}

void ReferenceNativeGpuCommandApi::set_next_present_status(
    NativePresentStatus status) noexcept {
    next_present_status_ = status;
}

NativePresentationScheduler::NativePresentationScheduler(
    NativePresentationConfig config,
    std::size_t metadata_hard_limit) noexcept
    : metadata_resource_(ledger_, core::ResourceClass::CompositorSurface),
      frames_(&metadata_resource_),
      config_(config) {
    ledger_.set_hard_limit(core::ResourceClass::CompositorSurface, metadata_hard_limit);
    try {
        frames_.resize(config.maximum_frames_in_flight);
    } catch (...) {
        frames_.clear();
    }
}

bool NativePresentationScheduler::clear() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_in_flight(frames_) != 0U) {
        return false;
    }
    surface_ = {};
    ++scheduler_generation_;
    next_ticket_id_ = 1U;
    completed_fence_value_ = 0U;
    last_submitted_fence_value_ = 0U;
    return scheduler_generation_ != 0U;
}

bool NativePresentationScheduler::retire_completed(
    std::uint64_t completed_fence_value,
    std::string* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (error != nullptr) {
        error->clear();
    }
    if (completed_fence_value < completed_fence_value_ ||
        completed_fence_value > last_submitted_fence_value_) {
        if (error != nullptr) {
            *error = "completed fence is outside the submitted timeline";
        }
        return false;
    }
    completed_fence_value_ = completed_fence_value;
    for (NativeInFlightFrameRecord& frame : frames_) {
        if (frame.occupied == 0U ||
            frame.receipt.signal_fence_value > completed_fence_value) {
            continue;
        }
        frame = {};
        ++retired_frames_;
    }
    return true;
}

NativePresentationSnapshot NativePresentationScheduler::snapshot_locked() const noexcept {
    NativePresentationSnapshot result;
    result.metadata = ledger_.snapshot(core::ResourceClass::CompositorSurface);
    result.config = config_;
    result.surface = surface_;
    result.scheduler_generation = scheduler_generation_;
    result.next_ticket_id = next_ticket_id_;
    result.completed_fence_value = completed_fence_value_;
    result.last_submitted_fence_value = last_submitted_fence_value_;
    result.configured_surfaces = configured_surfaces_;
    result.acquired_images = acquired_images_;
    result.submitted_frames = submitted_frames_;
    result.retired_frames = retired_frames_;
    result.skipped_frames = skipped_frames_;
    result.dropped_frames = dropped_frames_;
    result.out_of_date_events = out_of_date_events_;
    result.device_lost_events = device_lost_events_;
    result.stale_rejections = stale_rejections_;
    result.in_flight_frame_count = count_in_flight(frames_);
    return result;
}

NativePresentationSnapshot NativePresentationScheduler::snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_locked();
}

const char* native_present_error_kind_name(
    NativePresentErrorKind kind) noexcept {
    switch (kind) {
    case NativePresentErrorKind::None: return "none";
    case NativePresentErrorKind::InvalidInput: return "invalid-input";
    case NativePresentErrorKind::StaleCommandBuffer: return "stale-command-buffer";
    case NativePresentErrorKind::SurfaceConfigurationFailed: return "surface-configuration-failed";
    case NativePresentErrorKind::Backpressure: return "backpressure";
    case NativePresentErrorKind::SurfaceNotReady: return "surface-not-ready";
    case NativePresentErrorKind::SurfaceOutOfDate: return "surface-out-of-date";
    case NativePresentErrorKind::DeviceLost: return "device-lost";
    case NativePresentErrorKind::BackendFailure: return "backend-failure";
    case NativePresentErrorKind::FenceRegression: return "fence-regression";
    case NativePresentErrorKind::MetadataBudgetExceeded: return "metadata-budget-exceeded";
    case NativePresentErrorKind::AggregateOverflow: return "aggregate-overflow";
    }
    return "unknown";
}

bool submit_native_command_buffer(
    const NativePresentRequest& request,
    NativeGpuCommandApi* api,
    NativePresentationScheduler* scheduler,
    NativePresentReceipt* receipt,
    NativePresentStats* stats,
    NativePresentError* error) noexcept {
    if (receipt != nullptr) {
        *receipt = {};
    }
    if (stats != nullptr) {
        *stats = {};
    }
    if (error != nullptr) {
        *error = {};
    }
    if (request.commands == nullptr || request.frame == nullptr ||
        api == nullptr || scheduler == nullptr || receipt == nullptr ||
        !native_command_buffer_is_current(*request.frame, *request.commands)) {
        set_present_error(error, NativePresentErrorKind::InvalidInput, "invalid native present request");
        return false;
    }

    std::lock_guard<std::mutex> lock(scheduler->mutex_);
    if (stats != nullptr) {
        stats->metadata_before = scheduler->ledger_.snapshot(core::ResourceClass::CompositorSurface);
        stats->command_count = request.commands->commands.size();
        stats->damage_rect_count = request.commands->damage_rects.size();
        stats->wait_fence_value = request.wait_fence_value;
    }
    if (request.commands->surface != request.frame->surface ||
        request.commands->command_generation == 0U ||
        scheduler->config_.device_generation == 0U ||
        scheduler->config_.maximum_frames_in_flight == 0U ||
        scheduler->frames_.size() != scheduler->config_.maximum_frames_in_flight) {
        ++scheduler->stale_rejections_;
        set_present_error(error, NativePresentErrorKind::StaleCommandBuffer, "stale native command buffer");
        return false;
    }

    const std::uint64_t ticket_id = scheduler->next_ticket_id_;
    if (request.commands->commands.empty()) {
        receipt->frame_id = request.frame->frame_id;
        receipt->ticket_id = ticket_id;
        receipt->command_checksum = request.commands->command_checksum;
        receipt->status = NativePresentStatus::SkippedNoDamage;
        receipt->mode = scheduler->config_.present_mode;
        ++scheduler->next_ticket_id_;
        ++scheduler->skipped_frames_;
        if (stats != nullptr) {
            ++stats->skipped_frames;
            stats->metadata_after = scheduler->ledger_.snapshot(core::ResourceClass::CompositorSurface);
        }
        return true;
    }

    if (count_in_flight(scheduler->frames_) >= scheduler->config_.maximum_frames_in_flight) {
        if (scheduler->config_.drop_when_backpressured != 0U ||
            scheduler->config_.present_mode != NativePresentMode::Fifo) {
            receipt->frame_id = request.frame->frame_id;
            receipt->ticket_id = ticket_id;
            receipt->command_checksum = request.commands->command_checksum;
            receipt->command_count = static_cast<std::uint32_t>(request.commands->commands.size());
            receipt->damage_rect_count = static_cast<std::uint32_t>(request.commands->damage_rects.size());
            receipt->status = NativePresentStatus::DroppedBackpressure;
            receipt->mode = scheduler->config_.present_mode;
            ++scheduler->next_ticket_id_;
            ++scheduler->dropped_frames_;
            if (stats != nullptr) {
                ++stats->dropped_frames;
                stats->metadata_after = scheduler->ledger_.snapshot(core::ResourceClass::CompositorSurface);
            }
            return true;
        }
        set_present_error(error, NativePresentErrorKind::Backpressure, "native frame ring is full");
        return false;
    }

    if (scheduler->surface_ != request.frame->surface) {
        if (count_in_flight(scheduler->frames_) != 0U) {
            set_present_error(error, NativePresentErrorKind::Backpressure, "cannot reconfigure surface while frames are in flight");
            return false;
        }
        NativeGpuApiError backend_error;
        if (!api->configure_surface(
                request.frame->surface,
                scheduler->config_.swapchain_image_count,
                scheduler->config_.device_generation,
                &backend_error)) {
            set_present_error(
                error,
                NativePresentErrorKind::SurfaceConfigurationFailed,
                "native surface configuration failed",
                &backend_error);
            return false;
        }
        scheduler->surface_ = request.frame->surface;
        ++scheduler->configured_surfaces_;
    }

    NativeSwapchainImageHandle image;
    NativeAcquireStatus acquire_status = NativeAcquireStatus::NotReady;
    NativeGpuApiError backend_error;
    if (!api->acquire_next_image(
            scheduler->surface_,
            scheduler->config_.present_mode,
            ticket_id,
            &image,
            &acquire_status,
            &backend_error)) {
        set_present_error(error, NativePresentErrorKind::BackendFailure, "native acquire failed", &backend_error);
        return false;
    }
    if (acquire_status == NativeAcquireStatus::NotReady) {
        if (scheduler->config_.drop_when_backpressured != 0U ||
            scheduler->config_.present_mode != NativePresentMode::Fifo) {
            receipt->frame_id = request.frame->frame_id;
            receipt->ticket_id = ticket_id;
            receipt->command_checksum = request.commands->command_checksum;
            receipt->status = NativePresentStatus::DroppedBackpressure;
            receipt->mode = scheduler->config_.present_mode;
            ++scheduler->next_ticket_id_;
            ++scheduler->dropped_frames_;
            if (stats != nullptr) {
                ++stats->dropped_frames;
                stats->metadata_after = scheduler->ledger_.snapshot(core::ResourceClass::CompositorSurface);
            }
            return true;
        }
        set_present_error(error, NativePresentErrorKind::SurfaceNotReady, "no native swapchain image is ready");
        return false;
    }
    if (acquire_status == NativeAcquireStatus::OutOfDate) {
        ++scheduler->out_of_date_events_;
        set_present_error(error, NativePresentErrorKind::SurfaceOutOfDate, "native surface is out of date");
        return false;
    }
    if (acquire_status == NativeAcquireStatus::DeviceLost) {
        ++scheduler->device_lost_events_;
        set_present_error(error, NativePresentErrorKind::DeviceLost, "native device is lost");
        return false;
    }
    if (image.device_generation != scheduler->config_.device_generation ||
        image.surface_id != scheduler->surface_.surface_id ||
        image.surface_generation != scheduler->surface_.generation_id) {
        ++scheduler->stale_rejections_;
        set_present_error(error, NativePresentErrorKind::StaleCommandBuffer, "acquired image identity is stale");
        return false;
    }
    ++scheduler->acquired_images_;
    if (stats != nullptr) {
        ++stats->acquired_images;
    }

    std::uint64_t signal_fence = 0U;
    std::uint64_t encoded_checksum = 0U;
    NativePresentStatus present_status = NativePresentStatus::Presented;
    if (!api->encode_submit_present(
            image,
            *request.commands,
            *request.frame,
            request.draw_instances,
            ticket_id,
            request.wait_fence_value,
            &signal_fence,
            &encoded_checksum,
            &present_status,
            &backend_error)) {
        set_present_error(error, NativePresentErrorKind::BackendFailure, "native encode/present failed", &backend_error);
        return false;
    }
    if (present_status == NativePresentStatus::OutOfDate) {
        ++scheduler->out_of_date_events_;
        set_present_error(error, NativePresentErrorKind::SurfaceOutOfDate, "native present reported out-of-date surface");
        return false;
    }
    if (present_status == NativePresentStatus::DeviceLost) {
        ++scheduler->device_lost_events_;
        set_present_error(error, NativePresentErrorKind::DeviceLost, "native present reported device loss");
        return false;
    }
    if (present_status != NativePresentStatus::Presented ||
        signal_fence <= request.wait_fence_value ||
        signal_fence <= scheduler->last_submitted_fence_value_) {
        set_present_error(error, NativePresentErrorKind::FenceRegression, "native present fence is not monotone");
        return false;
    }

    NativeInFlightFrameRecord* slot = nullptr;
    for (NativeInFlightFrameRecord& frame : scheduler->frames_) {
        if (frame.occupied == 0U) {
            slot = &frame;
            break;
        }
    }
    if (slot == nullptr) {
        set_present_error(error, NativePresentErrorKind::MetadataBudgetExceeded, "native frame slot disappeared");
        return false;
    }
    receipt->image = image;
    receipt->frame_id = request.frame->frame_id;
    receipt->ticket_id = ticket_id;
    receipt->signal_fence_value = signal_fence;
    receipt->command_checksum = encoded_checksum;
    receipt->command_count = static_cast<std::uint32_t>(request.commands->commands.size());
    receipt->damage_rect_count = static_cast<std::uint32_t>(request.commands->damage_rects.size());
    receipt->status = NativePresentStatus::Presented;
    receipt->mode = scheduler->config_.present_mode;
    slot->receipt = *receipt;
    slot->occupied = 1U;
    ++scheduler->next_ticket_id_;
    scheduler->last_submitted_fence_value_ = signal_fence;
    ++scheduler->submitted_frames_;
    if (stats != nullptr) {
        ++stats->submitted_frames;
        stats->signal_fence_value = signal_fence;
        stats->metadata_after = scheduler->ledger_.snapshot(core::ResourceClass::CompositorSurface);
    }
    return true;
}

bool native_present_receipt_is_current(
    const NativePresentationScheduler& scheduler,
    const NativePresentReceipt& receipt) noexcept {
    std::lock_guard<std::mutex> lock(scheduler.mutex_);
    if (receipt.status != NativePresentStatus::Presented) {
        return receipt.ticket_id != 0U;
    }
    if (receipt.image.device_generation != scheduler.config_.device_generation ||
        receipt.image.surface_id != scheduler.surface_.surface_id ||
        receipt.image.surface_generation != scheduler.surface_.generation_id) {
        return false;
    }
    return std::any_of(
        scheduler.frames_.begin(),
        scheduler.frames_.end(),
        [&receipt](const NativeInFlightFrameRecord& frame) {
            return frame.occupied != 0U && frame.receipt == receipt;
        });
}

} // namespace zevryon::text
