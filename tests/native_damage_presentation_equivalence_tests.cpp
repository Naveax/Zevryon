#include "native_damage_presentation.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace {
using namespace zevryon::text;

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

bool overlap_or_touch(const NativeDamageRect& a, const NativeDamageRect& b, bool touching) {
    const std::int64_t ax = a.inline_start + static_cast<std::int64_t>(a.inline_size);
    const std::int64_t ay = a.block_start + static_cast<std::int64_t>(a.block_size);
    const std::int64_t bx = b.inline_start + static_cast<std::int64_t>(b.inline_size);
    const std::int64_t by = b.block_start + static_cast<std::int64_t>(b.block_size);
    if (touching) {
        return a.inline_start <= bx && b.inline_start <= ax &&
            a.block_start <= by && b.block_start <= ay;
    }
    return a.inline_start < bx && b.inline_start < ax &&
        a.block_start < by && b.block_start < ay;
}

NativeDamageRect unite(const NativeDamageRect& a, const NativeDamageRect& b) {
    const std::int64_t x0 = std::min(a.inline_start, b.inline_start);
    const std::int64_t y0 = std::min(a.block_start, b.block_start);
    const std::int64_t x1 = std::max(
        a.inline_start + static_cast<std::int64_t>(a.inline_size),
        b.inline_start + static_cast<std::int64_t>(b.inline_size));
    const std::int64_t y1 = std::max(
        a.block_start + static_cast<std::int64_t>(a.block_size),
        b.block_start + static_cast<std::int64_t>(b.block_size));
    return {x0, y0, static_cast<std::uint64_t>(x1 - x0), static_cast<std::uint64_t>(y1 - y0)};
}

void add_expected(
    std::vector<NativeDamageRect>* rects,
    NativeDamageRect rect,
    bool touching,
    std::uint32_t maximum_rects) {
    for (;;) {
        bool merged = false;
        for (std::size_t i = 0; i < rects->size(); ++i) {
            if (!overlap_or_touch((*rects)[i], rect, touching)) continue;
            rect = unite((*rects)[i], rect);
            rects->erase(rects->begin() + static_cast<std::ptrdiff_t>(i));
            merged = true;
            break;
        }
        if (!merged) break;
    }
    if (rects->size() >= maximum_rects) {
        for (const NativeDamageRect& existing : *rects) rect = unite(rect, existing);
        rects->clear();
    }
    rects->push_back(rect);
}

void sort_rects(std::vector<NativeDamageRect>* rects) {
    std::sort(rects->begin(), rects->end(), [](const auto& a, const auto& b) {
        return std::tie(a.inline_start, a.block_start, a.inline_size, a.block_size) <
            std::tie(b.inline_start, b.block_start, b.inline_size, b.block_size);
    });
}

bool intersects(const NativeDamageRect& a, const NativeDamageRect& b) {
    return overlap_or_touch(a, b, false);
}

GpuFrameSubmission make_frame(std::uint64_t frame_id, std::uint32_t mask) {
    GpuFrameSubmission frame;
    frame.surface = {77U, 91U, 640U, 320U, GpuSurfaceFormat::Bgra8Unorm, 1U, 0U, 0U};
    frame.frame_id = frame_id;
    frame.clips.push_back({0, 0, 640U, 320U});
    for (std::uint32_t index = 0U; index < 8U; ++index) {
        const std::int64_t shift = (mask & (1U << index)) != 0U ? 6 : 0;
        frame.fill_rects.push_back({
            20 + static_cast<std::int64_t>(index % 4U) * 140 + shift,
            20 + static_cast<std::int64_t>(index / 4U) * 120,
            60U,
            40U,
            index + 1U,
            index,
            index,
            0U});
        frame.commands.push_back({GpuFrameCommandKind::FillRect, index, 0U, 0U});
    }
    return frame;
}

NativeDamagePolicy make_policy(std::uint32_t variant) {
    NativeDamagePolicy policy;
    policy.maximum_rects = variant == 2U ? 3U : 32U;
    policy.maximum_commands = 256U;
    policy.full_redraw_threshold_permille = 0U;
    policy.flags = kNativeDamageCollapseOnOverflow;
    if (variant != 0U) policy.flags |= kNativeDamageMergeTouching;
    policy.maximum_total_area = 640U * 320U;
    return policy;
}

NativeDamageRect invalidation(std::uint32_t pattern) {
    const std::uint32_t column = pattern % 4U;
    const std::uint32_t row = pattern / 4U;
    return {
        static_cast<std::int64_t>(column * 140U + 78U),
        static_cast<std::int64_t>(row * 80U + 20U),
        18U,
        18U};
}

bool certify_case(
    std::uint32_t mask,
    std::uint32_t variant,
    std::uint32_t pattern) {
    GpuFrameSubmission previous_frame = make_frame(1U, 0U);
    NativeCommandBuffer previous;
    NativeCommandBuildError error;
    NativeDamagePolicy first_policy = make_policy(variant);
    first_policy.flags |= kNativeDamageForceFullRedraw;
    if (!build_native_command_buffer(
            {&previous_frame, {}, nullptr, {}, 1U, first_policy},
            &previous, nullptr, &error)) {
        return require(false, error.message);
    }

    GpuFrameSubmission current_frame = make_frame(2U, mask);
    const NativeDamageRect explicit_damage = invalidation(pattern);
    NativeCommandBuffer current;
    NativeCommandBuildStats stats;
    const NativeDamagePolicy policy = make_policy(variant);
    if (!build_native_command_buffer(
            {&current_frame, {}, &previous, std::span<const NativeDamageRect>(&explicit_damage, 1U), 2U, policy},
            &current, &stats, &error)) {
        return require(false, error.message);
    }

    std::vector<NativeDamageRect> expected;
    const bool touching = variant != 0U;
    for (std::uint32_t index = 0U; index < 8U; ++index) {
        if ((mask & (1U << index)) == 0U) continue;
        const TextPaintFillRect& old_fill = previous_frame.fill_rects[index];
        const TextPaintFillRect& new_fill = current_frame.fill_rects[index];
        add_expected(&expected, {
            old_fill.viewport_inline_start,
            old_fill.viewport_block_start,
            old_fill.inline_size,
            old_fill.block_size}, touching, policy.maximum_rects);
        add_expected(&expected, {
            new_fill.viewport_inline_start,
            new_fill.viewport_block_start,
            new_fill.inline_size,
            new_fill.block_size}, touching, policy.maximum_rects);
    }
    add_expected(&expected, explicit_damage, touching, policy.maximum_rects);

    std::vector<NativeDamageRect> actual(current.damage_rects.begin(), current.damage_rects.end());
    sort_rects(&expected);
    sort_rects(&actual);
    if (!require(actual == expected, "damage rectangle equivalence")) return false;

    std::uint64_t expected_draws = 0U;
    for (const NativeDamageRect& damage : expected) {
        for (const TextPaintFillRect& fill : current_frame.fill_rects) {
            const NativeDamageRect bounds{
                fill.viewport_inline_start,
                fill.viewport_block_start,
                fill.inline_size,
                fill.block_size};
            if (intersects(damage, bounds)) ++expected_draws;
        }
    }
    const std::uint64_t expected_commands = 2U + expected.size() + expected_draws;
    if (!require(current.commands.size() == expected_commands, "native command count equivalence") ||
        !require(stats.output_damage_rects == expected.size(), "stats damage count equivalence")) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    std::uint64_t passed = 0U;
    for (std::uint32_t mask = 0U; mask < 256U; ++mask) {
        for (std::uint32_t variant = 0U; variant < 3U; ++variant) {
            for (std::uint32_t pattern = 0U; pattern < 12U; ++pattern) {
                if (!certify_case(mask, variant, pattern)) return 1;
                ++passed;
            }
        }
    }
    constexpr std::uint64_t expected = 9'216U;
    if (!require(passed == expected, "exact oracle case count")) return 1;
    std::cout << "native damage presentation equivalence: " << passed << "/" << expected << " PASS\n";
    return 0;
}
