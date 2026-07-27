#include "native_damage_presentation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory_resource>
#include <string>
#include <vector>

namespace {
using namespace zevryon::text;

constexpr std::uint32_t kDocumentLines = 16'384U;
constexpr std::uint32_t kProjectedLines = 80U;
constexpr std::uint32_t kFillRects = 65U;
constexpr std::uint32_t kGlyphBatches = 3U;
constexpr std::uint32_t kDrawInstances = 310U;
constexpr std::uint32_t kFrameCommands = kFillRects + kGlyphBatches;
constexpr std::uint32_t kIterations = 512U;

void mix(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (std::uint32_t byte = 0U; byte < 8U; ++byte) {
        *hash ^= (value >> (byte * 8U)) & 0xffU;
        *hash *= 1'099'511'628'211ULL;
    }
}

struct Fixture final {
    GpuFrameSubmission frame;
    std::vector<GlyphAtlasDrawInstance> instances;

    Fixture() {
        frame.surface = {501U, 701U, 1'280U, 720U, GpuSurfaceFormat::Bgra8Unorm, 1U, 0U, 0U};
        frame.frame_id = 1U;
        frame.atlas_generation_id = 33U;
        frame.atlas_submission_epoch = 44U;
        frame.required_upload_fence = 3U;
        frame.clips.push_back({0, 0, 1'280U, 720U});
        for (std::uint32_t index = 0U; index < 64U; ++index) {
            const std::uint32_t column = index % 8U;
            const std::uint32_t row = index / 8U;
            frame.fill_rects.push_back({
                static_cast<std::int64_t>(column * 150U),
                static_cast<std::int64_t>(row * 80U),
                120U,
                50U,
                10U + index,
                index,
                index,
                1U});
            frame.commands.push_back({GpuFrameCommandKind::FillRect, index, 0U, 0U});
        }
        const std::array<std::uint32_t, 3> counts{104U, 103U, 103U};
        std::uint32_t first = 0U;
        for (std::uint32_t page = 0U; page < 3U; ++page) {
            for (std::uint32_t local = 0U; local < counts[page]; ++local) {
                const std::uint32_t global = first + local;
                instances.push_back({
                    static_cast<std::int64_t>((global % 40U) * 28U),
                    static_cast<std::int64_t>(100U + (global / 40U) * 42U),
                    33U,
                    100U + page,
                    page,
                    local % 16U,
                    local / 16U,
                    20U,
                    30U,
                    90U + page,
                    0U,
                    global});
            }
            frame.glyph_batches.push_back({
                100U + page,
                page,
                first,
                counts[page],
                90U + page,
                0U,
                page,
                1U});
            frame.commands.push_back({GpuFrameCommandKind::GlyphBatch, page, 0U, 0U});
            first += counts[page];
        }
        frame.fill_rects.push_back({620, 660, 2U, 28U, 200U, 79U, 319U, 2U});
        frame.commands.push_back({GpuFrameCommandKind::FillRect, 64U, 0U, 0U});
    }
};

NativeDamagePolicy policy() {
    return {
        16U,
        512U,
        850U,
        kNativeDamageMergeTouching | kNativeDamageCollapseOnOverflow,
        1'280U * 720U,
        0U};
}

struct Result final {
    NativeCommandBuildStats cold_build;
    NativeCommandBuildStats hot_build;
    NativeCommandBuildStats partial_build;
    NativePresentReceipt cold_receipt;
    NativePresentReceipt hot_receipt;
    NativePresentReceipt partial_receipt;
    NativePresentationSnapshot snapshot;
    std::uint64_t cold_retained_bytes{0};
    std::uint64_t hot_retained_bytes{0};
    std::uint64_t partial_retained_bytes{0};
    std::uint64_t checksum{0};
};

std::uint64_t retained_bytes(const NativeCommandBuffer& buffer) {
    return static_cast<std::uint64_t>(buffer.damage_rects.size()) * sizeof(NativeDamageRect) +
        static_cast<std::uint64_t>(buffer.footprints.size()) * sizeof(NativeCommandFootprint) +
        static_cast<std::uint64_t>(buffer.commands.size()) * sizeof(NativeCommandRecord);
}

bool execute(Result* result) {
    Fixture fixture;
    NativeCommandBuffer cold;
    NativeCommandBuildError build_error;
    if (!build_native_command_buffer(
            {&fixture.frame, fixture.instances, nullptr, {}, 1U, policy()},
            &cold, &result->cold_build, &build_error)) {
        std::cerr << build_error.message << '\n';
        return false;
    }
    ReferenceNativeGpuCommandApi api;
    NativePresentationScheduler scheduler(
        {3U, 3U, NativePresentMode::Fifo, 0U, 0U, 99U, 701U},
        512U);
    NativePresentError present_error;
    if (!submit_native_command_buffer(
            {&cold, &fixture.frame, fixture.instances, 3U},
            &api, &scheduler, &result->cold_receipt, nullptr, &present_error)) {
        std::cerr << present_error.message << '\n';
        return false;
    }
    std::string retire_error;
    if (!scheduler.retire_completed(result->cold_receipt.signal_fence_value, &retire_error)) {
        std::cerr << retire_error << '\n';
        return false;
    }

    fixture.frame.frame_id = 2U;
    NativeCommandBuffer hot;
    if (!build_native_command_buffer(
            {&fixture.frame, fixture.instances, &cold, {}, 2U, policy()},
            &hot, &result->hot_build, &build_error)) {
        std::cerr << build_error.message << '\n';
        return false;
    }
    if (!submit_native_command_buffer(
            {&hot, &fixture.frame, fixture.instances, 0U},
            &api, &scheduler, &result->hot_receipt, nullptr, &present_error)) {
        std::cerr << present_error.message << '\n';
        return false;
    }

    fixture.frame.frame_id = 3U;
    const std::array<NativeDamageRect, 4> invalidations{{
        {0, 0, 160U, 80U},
        {450, 160, 160U, 80U},
        {900, 320, 160U, 80U},
        {600, 640, 80U, 60U}}};
    NativeCommandBuffer partial;
    if (!build_native_command_buffer(
            {&fixture.frame, fixture.instances, &hot, invalidations, 3U, policy()},
            &partial, &result->partial_build, &build_error)) {
        std::cerr << build_error.message << '\n';
        return false;
    }
    if (!submit_native_command_buffer(
            {&partial, &fixture.frame, fixture.instances, result->cold_receipt.signal_fence_value},
            &api, &scheduler, &result->partial_receipt, nullptr, &present_error)) {
        std::cerr << present_error.message << '\n';
        return false;
    }

    result->snapshot = scheduler.snapshot();
    result->cold_retained_bytes = retained_bytes(cold);
    result->hot_retained_bytes = retained_bytes(hot);
    result->partial_retained_bytes = retained_bytes(partial);
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    mix(&hash, cold.command_checksum);
    mix(&hash, hot.command_checksum);
    mix(&hash, partial.command_checksum);
    mix(&hash, result->cold_receipt.signal_fence_value);
    mix(&hash, result->hot_receipt.ticket_id);
    mix(&hash, result->partial_receipt.signal_fence_value);
    mix(&hash, result->partial_receipt.command_checksum);
    mix(&hash, result->snapshot.metadata.current_bytes);
    mix(&hash, result->snapshot.metadata.peak_bytes);
    mix(&hash, result->snapshot.submitted_frames);
    mix(&hash, result->snapshot.skipped_frames);
    mix(&hash, result->snapshot.retired_frames);
    mix(&hash, result->cold_retained_bytes);
    mix(&hash, result->hot_retained_bytes);
    mix(&hash, result->partial_retained_bytes);
    result->checksum = hash;
    return true;
}

double percentile(const std::vector<double>& samples, double p) {
    const std::size_t index = static_cast<std::size_t>(
        p * static_cast<double>(samples.size() - 1U));
    return samples[index];
}

} // namespace

int main() {
    Result baseline;
    if (!execute(&baseline)) return 1;
    std::vector<double> samples;
    samples.reserve(kIterations);
    for (std::uint32_t iteration = 0U; iteration < kIterations; ++iteration) {
        Result measured;
        const auto start = std::chrono::steady_clock::now();
        if (!execute(&measured)) return 1;
        const auto end = std::chrono::steady_clock::now();
        if (measured.checksum != baseline.checksum) return 2;
        samples.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    std::sort(samples.begin(), samples.end());
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "{\n";
    std::cout << "  \"schema\": \"zevryon.native-damage-presentation-benchmark.v1\",\n";
    std::cout << "  \"input_document_lines\": " << kDocumentLines << ",\n";
    std::cout << "  \"input_projected_lines\": " << kProjectedLines << ",\n";
    std::cout << "  \"input_frame_commands\": " << kFrameCommands << ",\n";
    std::cout << "  \"input_draw_instances\": " << kDrawInstances << ",\n";
    std::cout << "  \"cold_damage_rects\": " << baseline.cold_build.output_damage_rects << ",\n";
    std::cout << "  \"cold_native_commands\": " << baseline.cold_build.output_commands << ",\n";
    std::cout << "  \"hot_damage_rects\": " << baseline.hot_build.output_damage_rects << ",\n";
    std::cout << "  \"hot_native_commands\": " << baseline.hot_build.output_commands << ",\n";
    std::cout << "  \"partial_damage_rects\": " << baseline.partial_build.output_damage_rects << ",\n";
    std::cout << "  \"partial_native_commands\": " << baseline.partial_build.output_commands << ",\n";
    std::cout << "  \"partial_culled_commands\": " << baseline.partial_build.culled_commands << ",\n";
    std::cout << "  \"partial_duplicated_commands\": " << baseline.partial_build.duplicated_commands << ",\n";
    std::cout << "  \"cold_present_status\": " << static_cast<std::uint32_t>(baseline.cold_receipt.status) << ",\n";
    std::cout << "  \"hot_present_status\": " << static_cast<std::uint32_t>(baseline.hot_receipt.status) << ",\n";
    std::cout << "  \"partial_present_status\": " << static_cast<std::uint32_t>(baseline.partial_receipt.status) << ",\n";
    std::cout << "  \"cold_signal_fence\": " << baseline.cold_receipt.signal_fence_value << ",\n";
    std::cout << "  \"partial_signal_fence\": " << baseline.partial_receipt.signal_fence_value << ",\n";
    std::cout << "  \"submitted_frames\": " << baseline.snapshot.submitted_frames << ",\n";
    std::cout << "  \"skipped_frames\": " << baseline.snapshot.skipped_frames << ",\n";
    std::cout << "  \"retired_frames\": " << baseline.snapshot.retired_frames << ",\n";
    std::cout << "  \"in_flight_frames\": " << baseline.snapshot.in_flight_frame_count << ",\n";
    std::cout << "  \"scheduler_current_bytes\": " << baseline.snapshot.metadata.current_bytes << ",\n";
    std::cout << "  \"scheduler_peak_bytes\": " << baseline.snapshot.metadata.peak_bytes << ",\n";
    std::cout << "  \"scheduler_hard_limit_bytes\": " << baseline.snapshot.metadata.hard_limit_bytes << ",\n";
    std::cout << "  \"cold_retained_bytes\": " << baseline.cold_retained_bytes << ",\n";
    std::cout << "  \"hot_retained_bytes\": " << baseline.hot_retained_bytes << ",\n";
    std::cout << "  \"partial_retained_bytes\": " << baseline.partial_retained_bytes << ",\n";
    std::cout << "  \"damage_rect_bytes\": " << sizeof(NativeDamageRect) << ",\n";
    std::cout << "  \"footprint_bytes\": " << sizeof(NativeCommandFootprint) << ",\n";
    std::cout << "  \"command_record_bytes\": " << sizeof(NativeCommandRecord) << ",\n";
    std::cout << "  \"present_receipt_bytes\": " << sizeof(NativePresentReceipt) << ",\n";
    std::cout << "  \"in_flight_record_bytes\": " << sizeof(NativeInFlightFrameRecord) << ",\n";
    std::cout << "  \"checksum\": " << baseline.checksum << ",\n";
    std::cout << "  \"p50_ms\": " << percentile(samples, 0.50) << ",\n";
    std::cout << "  \"p95_ms\": " << percentile(samples, 0.95) << ",\n";
    std::cout << "  \"p99_ms\": " << percentile(samples, 0.99) << ",\n";
    std::cout << "  \"maximum_ms\": " << samples.back() << "\n";
    std::cout << "}\n";
    return 0;
}
