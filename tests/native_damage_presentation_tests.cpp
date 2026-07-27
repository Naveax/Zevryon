#include "native_damage_presentation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <new>
#include <span>
#include <string>
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

class BoundedResource final : public std::pmr::memory_resource {
public:
    explicit BoundedResource(std::size_t limit) : limit_(limit) {}
    std::size_t current() const noexcept { return current_; }
    std::size_t peak() const noexcept { return peak_; }
private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (bytes > limit_ - current_) {
            throw std::bad_alloc();
        }
        void* ptr = std::pmr::get_default_resource()->allocate(bytes, alignment);
        current_ += bytes;
        if (current_ > peak_) peak_ = current_;
        return ptr;
    }
    void do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) override {
        std::pmr::get_default_resource()->deallocate(ptr, bytes, alignment);
        current_ -= bytes;
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::size_t limit_{};
    std::size_t current_{};
    std::size_t peak_{};
};

struct Fixture final {
    GpuFrameSubmission frame;
    std::vector<GlyphAtlasDrawInstance> instances;

    Fixture() {
        frame.surface = {7U, 11U, 800U, 600U, GpuSurfaceFormat::Bgra8Unorm, 1U, 0U, 0U};
        frame.frame_id = 1U;
        frame.atlas_generation_id = 5U;
        frame.atlas_submission_epoch = 9U;
        frame.required_upload_fence = 3U;
        frame.clips.push_back({0, 0, 800U, 600U});
        frame.fill_rects.push_back({10, 20, 100U, 30U, 1U, 2U, 3U, 0U});
        frame.fill_rects.push_back({300, 400, 2U, 20U, 2U, 9U, 10U, 0U});
        instances.push_back({150, 100, 5U, 2U, 0U, 0U, 0U, 20U, 30U, 4U, 0U, 0U});
        instances.push_back({172, 100, 5U, 2U, 0U, 20U, 0U, 18U, 30U, 4U, 0U, 1U});
        frame.glyph_batches.push_back({2U, 0U, 0U, 2U, 4U, 0U, 0U, 1U});
        frame.commands.push_back({GpuFrameCommandKind::FillRect, 0U, 0U, 0U});
        frame.commands.push_back({GpuFrameCommandKind::GlyphBatch, 0U, 0U, 0U});
        frame.commands.push_back({GpuFrameCommandKind::FillRect, 1U, 0U, 0U});
    }
};

NativeDamagePolicy policy(std::uint32_t flags = kNativeDamageMergeTouching | kNativeDamageCollapseOnOverflow) {
    return {16U, 128U, 900U, flags, 800U * 600U, 0U};
}

bool build(
    Fixture* fixture,
    std::uint64_t generation,
    const NativeCommandBuffer* previous,
    std::span<const NativeDamageRect> invalidations,
    NativeCommandBuffer* output,
    NativeCommandBuildStats* stats = nullptr,
    NativeCommandBuildError* error = nullptr,
    NativeDamagePolicy selected_policy = policy()) {
    return build_native_command_buffer(
        {&fixture->frame, fixture->instances, previous, invalidations, generation, selected_policy},
        output,
        stats,
        error);
}

bool test_full_then_no_damage() {
    Fixture fixture;
    NativeCommandBuffer first;
    NativeCommandBuildStats first_stats;
    NativeCommandBuildError error;
    if (!require(build(&fixture, 1U, nullptr, {}, &first, &first_stats, &error), error.message) ||
        !require(first.full_redraw == 1U, "first frame full redraw") ||
        !require(first.damage_rects.size() == 1U, "one full-surface damage rect") ||
        !require(first.damage_rects.front() == NativeDamageRect{0, 0, 800U, 600U}, "full surface damage") ||
        !require(first.commands.size() == 6U, "begin/scissor/three draws/end") ||
        !require(first.commands[0].kind == NativeCommandKind::BeginRenderPass, "begin pass first") ||
        !require(first.commands[1].kind == NativeCommandKind::SetScissor, "scissor second") ||
        !require(first.commands[2].kind == NativeCommandKind::FillRect, "selection fill order") ||
        !require(first.commands[3].kind == NativeCommandKind::GlyphBatch, "glyph order") ||
        !require(first.commands[4].kind == NativeCommandKind::FillRect, "caret order") ||
        !require(first.commands[5].kind == NativeCommandKind::EndRenderPass, "end pass last")) {
        return false;
    }
    fixture.frame.frame_id = 2U;
    NativeCommandBuffer second;
    NativeCommandBuildStats second_stats;
    if (!require(build(&fixture, 2U, &first, {}, &second, &second_stats, &error), error.message) ||
        !require(second.full_redraw == 0U, "identical frame is not full redraw") ||
        !require(second.damage_rects.empty(), "identical frame has no damage") ||
        !require(second.commands.empty(), "identical frame emits no native commands") ||
        !require(second.footprints.size() == first.footprints.size(), "history retained")) {
        return false;
    }
    return true;
}

bool test_changed_and_removed_damage() {
    Fixture fixture;
    NativeCommandBuffer previous;
    NativeCommandBuildError error;
    if (!build(&fixture, 1U, nullptr, {}, &previous, nullptr, &error)) return false;
    fixture.frame.frame_id = 2U;
    fixture.frame.fill_rects[0].viewport_inline_start = 40;
    fixture.frame.commands.pop_back();
    NativeCommandBuffer current;
    NativeCommandBuildStats stats;
    if (!require(build(&fixture, 2U, &previous, {}, &current, &stats, &error), error.message) ||
        !require(stats.changed_commands == 1U, "one changed command") ||
        !require(stats.removed_commands == 1U, "one removed command") ||
        !require(!current.damage_rects.empty(), "changed/removed damage emitted") ||
        !require(current.commands.front().kind == NativeCommandKind::BeginRenderPass, "partial begins pass") ||
        !require(current.commands.back().kind == NativeCommandKind::EndRenderPass, "partial ends pass")) {
        return false;
    }
    bool old_selection_covered = false;
    bool new_selection_covered = false;
    bool removed_caret_covered = false;
    for (const NativeDamageRect& rect : current.damage_rects) {
        const auto contains = [&rect](std::int64_t x, std::int64_t y) {
            return x >= rect.inline_start && y >= rect.block_start &&
                x < rect.inline_start + static_cast<std::int64_t>(rect.inline_size) &&
                y < rect.block_start + static_cast<std::int64_t>(rect.block_size);
        };
        old_selection_covered = old_selection_covered || contains(10, 20);
        new_selection_covered = new_selection_covered || contains(40, 20);
        removed_caret_covered = removed_caret_covered || contains(300, 400);
    }
    return require(old_selection_covered, "old bounds damaged") &&
        require(new_selection_covered, "new bounds damaged") &&
        require(removed_caret_covered, "removed command bounds damaged");
}

bool test_explicit_damage_and_threshold() {
    Fixture fixture;
    NativeCommandBuffer previous;
    NativeCommandBuildError error;
    if (!build(&fixture, 1U, nullptr, {}, &previous, nullptr, &error)) return false;
    fixture.frame.frame_id = 2U;
    const std::array<NativeDamageRect, 2> invalidations{{
        {0, 0, 20U, 20U},
        {20, 0, 20U, 20U}}};
    NativeCommandBuffer current;
    NativeCommandBuildStats stats;
    if (!require(build(&fixture, 2U, &previous, invalidations, &current, &stats, &error), error.message) ||
        !require(current.damage_rects.size() == 1U, "touching invalidations merged") ||
        !require(current.damage_rects[0] == NativeDamageRect{0, 0, 40U, 20U}, "merged explicit damage bounds") ||
        !require(stats.explicit_invalidations == 2U, "explicit invalidation count")) {
        return false;
    }
    fixture.frame.frame_id = 3U;
    const std::array<NativeDamageRect, 1> large{{{0, 0, 760U, 580U}}};
    NativeCommandBuffer full;
    NativeDamagePolicy p = policy();
    p.full_redraw_threshold_permille = 800U;
    if (!require(build(&fixture, 3U, &current, large, &full, nullptr, &error, p), error.message) ||
        !require(full.full_redraw == 1U, "area threshold forces full redraw") ||
        !require(full.damage_rects.size() == 1U && full.damage_rects[0].inline_size == 800U, "threshold full surface")) {
        return false;
    }
    return true;
}

bool test_failure_atomicity_and_budget() {
    Fixture fixture;
    fixture.frame.commands[1].payload_index = 99U;
    NativeCommandBuffer output;
    NativeCommandBuildError error;
    if (!require(!build(&fixture, 1U, nullptr, {}, &output, nullptr, &error), "invalid topology rejected") ||
        !require(error.kind == NativeCommandBuildErrorKind::FrameTopologyViolation, "topology error kind") ||
        !require(output.commands.empty() && output.damage_rects.empty() && output.footprints.empty(), "failure atomic empty output")) {
        return false;
    }
    Fixture valid;
    BoundedResource resource(1U);
    NativeCommandBuffer limited(&resource);
    if (!require(!build(&valid, 1U, nullptr, {}, &limited, nullptr, &error), "tiny PMR budget rejected") ||
        !require(error.kind == NativeCommandBuildErrorKind::OutputBudgetExceeded, "budget error kind") ||
        !require(limited.commands.empty(), "budget failure empty output") ||
        !require(resource.current() == 0U, "budget failure releases memory")) {
        return false;
    }
    return true;
}

bool test_presentation_and_retirement() {
    Fixture fixture;
    NativeCommandBuffer commands;
    NativeCommandBuildError build_error;
    if (!build(&fixture, 1U, nullptr, {}, &commands, nullptr, &build_error)) return false;
    ReferenceNativeGpuCommandApi api;
    NativePresentationScheduler scheduler({2U, 3U, NativePresentMode::Fifo, 0U, 0U, 41U, 11U}, 1U << 20U);
    NativePresentReceipt receipt;
    NativePresentStats stats;
    NativePresentError error;
    if (!require(submit_native_command_buffer(
            {&commands, &fixture.frame, fixture.instances, 3U},
            &api, &scheduler, &receipt, &stats, &error), error.message) ||
        !require(receipt.status == NativePresentStatus::Presented, "presented status") ||
        !require(receipt.signal_fence_value > 3U, "signal after wait") ||
        !require(native_present_receipt_is_current(scheduler, receipt), "receipt current while in flight") ||
        !require(scheduler.snapshot().in_flight_frame_count == 1U, "one frame in flight")) {
        return false;
    }
    std::string retire_error;
    if (!require(scheduler.retire_completed(receipt.signal_fence_value, &retire_error), retire_error) ||
        !require(!native_present_receipt_is_current(scheduler, receipt), "retired receipt no longer current") ||
        !require(scheduler.snapshot().retired_frames == 1U, "retired count")) {
        return false;
    }
    return true;
}

bool test_skip_backpressure_and_surface_states() {
    Fixture fixture;
    NativeCommandBuffer first;
    NativeCommandBuildError build_error;
    if (!build(&fixture, 1U, nullptr, {}, &first, nullptr, &build_error)) return false;
    fixture.frame.frame_id = 2U;
    NativeCommandBuffer no_damage;
    if (!build(&fixture, 2U, &first, {}, &no_damage, nullptr, &build_error)) return false;
    ReferenceNativeGpuCommandApi api;
    NativePresentationScheduler scheduler({1U, 2U, NativePresentMode::Fifo, 0U, 0U, 9U, 11U}, 1U << 20U);
    NativePresentReceipt receipt;
    NativePresentError error;
    if (!require(submit_native_command_buffer(
            {&no_damage, &fixture.frame, fixture.instances, 0U},
            &api, &scheduler, &receipt, nullptr, &error), error.message) ||
        !require(receipt.status == NativePresentStatus::SkippedNoDamage, "no-damage frame skipped")) {
        return false;
    }

    fixture.frame.frame_id = 3U;
    NativeCommandBuffer damaged;
    const std::array<NativeDamageRect, 1> invalidation{{{0, 0, 10U, 10U}}};
    if (!build(&fixture, 3U, &no_damage, invalidation, &damaged, nullptr, &build_error)) return false;
    if (!require(submit_native_command_buffer(
            {&damaged, &fixture.frame, fixture.instances, 0U},
            &api, &scheduler, &receipt, nullptr, &error), error.message)) {
        return false;
    }
    fixture.frame.frame_id = 4U;
    NativeCommandBuffer second_damage;
    if (!build(&fixture, 4U, &damaged, invalidation, &second_damage, nullptr, &build_error)) return false;
    NativePresentReceipt second_receipt;
    if (!require(!submit_native_command_buffer(
            {&second_damage, &fixture.frame, fixture.instances, 0U},
            &api, &scheduler, &second_receipt, nullptr, &error), "FIFO backpressure rejected") ||
        !require(error.kind == NativePresentErrorKind::Backpressure, "backpressure kind")) {
        return false;
    }

    ReferenceNativeGpuCommandApi mailbox_api;
    NativePresentationScheduler mailbox({1U, 2U, NativePresentMode::Mailbox, 1U, 0U, 9U, 11U}, 1U << 20U);
    fixture.frame.frame_id = 5U;
    NativeCommandBuffer mailbox_first;
    if (!build(&fixture, 5U, nullptr, {}, &mailbox_first, nullptr, &build_error)) return false;
    if (!submit_native_command_buffer({&mailbox_first, &fixture.frame, fixture.instances, 0U}, &mailbox_api, &mailbox, &receipt, nullptr, &error)) return false;
    fixture.frame.frame_id = 6U;
    NativeCommandBuffer mailbox_second;
    if (!build(&fixture, 6U, &mailbox_first, invalidation, &mailbox_second, nullptr, &build_error)) return false;
    if (!require(submit_native_command_buffer({&mailbox_second, &fixture.frame, fixture.instances, 0U}, &mailbox_api, &mailbox, &second_receipt, nullptr, &error), error.message) ||
        !require(second_receipt.status == NativePresentStatus::DroppedBackpressure, "mailbox drops under backpressure")) {
        return false;
    }

    ReferenceNativeGpuCommandApi status_api;
    NativePresentationScheduler status_scheduler({2U, 2U, NativePresentMode::Fifo, 0U, 0U, 9U, 11U}, 1U << 20U);
    fixture.frame.frame_id = 7U;
    NativeCommandBuffer status_commands;
    if (!build(&fixture, 7U, nullptr, {}, &status_commands, nullptr, &build_error)) return false;
    status_api.set_next_acquire_status(NativeAcquireStatus::OutOfDate);
    if (!require(!submit_native_command_buffer({&status_commands, &fixture.frame, fixture.instances, 0U}, &status_api, &status_scheduler, &receipt, nullptr, &error), "out-of-date rejected") ||
        !require(error.kind == NativePresentErrorKind::SurfaceOutOfDate, "out-of-date kind")) {
        return false;
    }
    status_api.set_next_acquire_status(NativeAcquireStatus::DeviceLost);
    if (!require(!submit_native_command_buffer({&status_commands, &fixture.frame, fixture.instances, 0U}, &status_api, &status_scheduler, &receipt, nullptr, &error), "device lost rejected") ||
        !require(error.kind == NativePresentErrorKind::DeviceLost, "device-lost kind")) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    const std::array tests{
        test_full_then_no_damage,
        test_changed_and_removed_damage,
        test_explicit_damage_and_threshold,
        test_failure_atomicity_and_budget,
        test_presentation_and_retirement,
        test_skip_backpressure_and_surface_states};
    for (const auto test : tests) {
        if (!test()) return 1;
    }
    std::cout << "native damage presentation tests: PASS\n";
    return 0;
}
