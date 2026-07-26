#include "gpu_device_presentation.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace {
using namespace zevryon::text;

constexpr std::uint32_t kDocumentLines = 16'384U;
constexpr std::uint32_t kProjectedLines = 80U;
constexpr std::uint32_t kUploads = 93U;
constexpr std::uint32_t kDrawInstances = 310U;
constexpr std::uint32_t kDrawBatches = 3U;
constexpr std::uint32_t kSelectionCommands = 64U;
constexpr std::uint32_t kCaretCommands = 1U;
constexpr std::uint64_t kPayloadBytes = 12'720U;

void mix(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (std::uint32_t byte = 0U; byte < 8U; ++byte) {
        *hash ^= (value >> (byte * 8U)) & 0xffU;
        *hash *= 1'099'511'628'211ULL;
    }
}

double percentile(std::vector<double> values, double probability) {
    std::sort(values.begin(), values.end());
    const double position = probability * static_cast<double>(values.size() - 1U);
    const std::size_t lower = static_cast<std::size_t>(std::floor(position));
    const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
    const double fraction = position - static_cast<double>(lower);
    return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

struct Fixture final {
    std::vector<std::byte> payload;
    std::vector<GlyphAtlasUploadRecord> uploads;
    std::array<GlyphAtlasBackendUploadBatch, 3> batches{};
    std::vector<GlyphAtlasDrawInstance> instances;
    GpuFrameSubmission frame;

    Fixture() {
        payload.resize(kPayloadBytes);
        for (std::size_t index = 0U; index < payload.size(); ++index) {
            payload[index] = static_cast<std::byte>((index * 29U + 17U) & 0xffU);
        }
        uploads.reserve(kUploads);
        constexpr std::array<GlyphRasterFormat, 3> formats{
            GlyphRasterFormat::Alpha8,
            GlyphRasterFormat::LcdRgb8,
            GlyphRasterFormat::Bgra8};
        std::uint64_t payload_offset = 0U;
        for (std::uint32_t page = 0U; page < 3U; ++page) {
            const std::uint32_t first_upload = static_cast<std::uint32_t>(uploads.size());
            for (std::uint32_t local = 0U; local < 31U; ++local) {
                const std::uint64_t size = local == 30U ? 160U : 136U;
                GlyphAtlasUploadRecord upload;
                upload.atlas_generation_id = 17U;
                upload.page_generation = 100U + page;
                upload.payload_offset = payload_offset;
                upload.payload_size = size;
                upload.page_index = page;
                upload.atlas_x = (local % 8U) * 8U;
                upload.atlas_y = (local / 8U) * 8U;
                upload.width = 8U;
                upload.height = 8U;
                upload.row_bytes = static_cast<std::uint32_t>(size / 8U);
                upload.working_set_key_index = page * 31U + local;
                upload.format = formats[page];
                uploads.push_back(upload);
                payload_offset += size;
            }
            batches[page] = {
                17U,
                100U + page,
                page,
                first_upload,
                31U,
                formats[page],
                0U,
                0U};
        }

        instances.reserve(kDrawInstances);
        for (std::uint32_t index = 0U; index < kDrawInstances; ++index) {
            const std::uint32_t page = index < 104U ? 0U : (index < 207U ? 1U : 2U);
            GlyphAtlasDrawInstance instance;
            instance.viewport_inline_start = static_cast<std::int64_t>((index % 80U) * 9U);
            instance.viewport_block_start = static_cast<std::int64_t>((index / 80U) * 18U);
            instance.atlas_generation_id = 17U;
            instance.page_generation = 100U + page;
            instance.page_index = page;
            instance.atlas_x = (index % 8U) * 8U;
            instance.atlas_y = ((index / 8U) % 8U) * 8U;
            instance.width = 8U;
            instance.height = 8U;
            instance.style_id = 30U + page;
            instance.clip_index = 0U;
            instance.working_set_key_index = index % 93U;
            instances.push_back(instance);
        }

        frame.surface = {500U, 700U, 1280U, 720U, GpuSurfaceFormat::Bgra8Unorm, 1U, 0U, 0U};
        frame.frame_id = 1U;
        frame.atlas_generation_id = 17U;
        frame.atlas_submission_epoch = 1U;
        frame.required_upload_fence = 3U;
        frame.clips.push_back({0, 0, 1280U, 720U});
        for (std::uint32_t index = 0U; index < kSelectionCommands; ++index) {
            frame.fill_rects.push_back({
                static_cast<std::int64_t>((index % 16U) * 30U),
                static_cast<std::int64_t>((index / 16U) * 18U),
                24U,
                16U,
                1U,
                index,
                index,
                kTextPaintRectSelection});
            frame.commands.push_back({GpuFrameCommandKind::FillRect, index, 0U, 0U});
        }
        constexpr std::array<std::uint32_t, 3> firsts{0U, 104U, 207U};
        constexpr std::array<std::uint32_t, 3> counts{104U, 103U, 103U};
        for (std::uint32_t page = 0U; page < 3U; ++page) {
            frame.page_references.push_back({
                100U + page,
                1U + page,
                page,
                page,
                1U,
                formats[page],
                0U,
                0U});
            frame.glyph_batches.push_back({
                100U + page,
                page,
                firsts[page],
                counts[page],
                30U + page,
                0U,
                page,
                0U});
            frame.commands.push_back({GpuFrameCommandKind::GlyphBatch, page, 0U, 0U});
        }
        frame.fill_rects.push_back({600, 200, 1U, 18U, 2U, 79U, 319U, kTextPaintRectCaret});
        frame.commands.push_back({GpuFrameCommandKind::FillRect, kSelectionCommands, 0U, 0U});
    }
};

struct RunResult final {
    GpuDevicePresentationSnapshot snapshot;
    GpuPresentReceipt receipt;
    std::uint64_t checksum{0U};
};

bool execute(Fixture* fixture, RunResult* result) {
    ReferenceGpuDeviceApi api;
    GpuDevicePresentationBackend backend(
        &api,
        {3U, 3U, 2U, 6U, 64U, 64U, 90U},
        1'104U);
    std::array<std::uint64_t, 3> upload_fences{};
    for (std::uint32_t index = 0U; index < 3U; ++index) {
        GlyphAtlasUploadBackendError error;
        if (!backend.submit(
                fixture->batches[index],
                fixture->uploads,
                fixture->payload,
                index + 1U,
                &upload_fences[index],
                &error)) {
            std::cerr << error.message << '\n';
            return false;
        }
    }
    GpuFrameBackendError frame_error;
    std::uint64_t cold_fence = 0U;
    if (!backend.submit(
            fixture->frame,
            fixture->instances,
            4U,
            3U,
            &cold_fence,
            &frame_error)) {
        std::cerr << frame_error.message << '\n';
        return false;
    }
    std::string retire_error;
    if (!backend.retire_completed(cold_fence, &retire_error)) {
        std::cerr << retire_error << '\n';
        return false;
    }

    fixture->frame.frame_id = 2U;
    fixture->frame.required_upload_fence = 0U;
    for (GpuFramePageReference& page : fixture->frame.page_references) {
        page.required_upload_fence = 0U;
    }
    std::uint64_t hot_fence = 0U;
    if (!backend.submit(
            fixture->frame,
            fixture->instances,
            5U,
            0U,
            &hot_fence,
            &frame_error)) {
        std::cerr << frame_error.message << '\n';
        return false;
    }
    result->snapshot = backend.snapshot();
    if (!backend.latest_present_receipt(&result->receipt)) {
        return false;
    }
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    mix(&hash, cold_fence);
    mix(&hash, hot_fence);
    mix(&hash, result->receipt.command_checksum);
    mix(&hash, result->receipt.command_count);
    mix(&hash, result->receipt.image.image_generation);
    mix(&hash, result->snapshot.metadata.current_bytes);
    mix(&hash, result->snapshot.metadata.peak_bytes);
    mix(&hash, result->snapshot.last_submitted_fence_value);
    mix(&hash, result->snapshot.texture_count);
    mix(&hash, result->snapshot.surface_image_count);
    mix(&hash, result->snapshot.in_flight_frame_count);
    mix(&hash, result->snapshot.texture_pin_count);
    mix(&hash, result->snapshot.upload_submissions);
    mix(&hash, result->snapshot.present_submissions);
    result->checksum = hash;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::size_t iterations = 512U;
    if (argc == 2) {
        iterations = static_cast<std::size_t>(std::stoull(argv[1]));
    }
    Fixture fixture;
    RunResult baseline;
    if (!execute(&fixture, &baseline)) {
        return 1;
    }
    std::vector<double> samples;
    samples.reserve(iterations);
    for (std::size_t iteration = 0U; iteration < iterations; ++iteration) {
        Fixture measured_fixture;
        RunResult result;
        const auto start = std::chrono::steady_clock::now();
        if (!execute(&measured_fixture, &result)) {
            return 1;
        }
        const auto finish = std::chrono::steady_clock::now();
        if (result.checksum != baseline.checksum ||
            result.snapshot.metadata.current_bytes != baseline.snapshot.metadata.current_bytes ||
            result.snapshot.metadata.peak_bytes != baseline.snapshot.metadata.peak_bytes) {
            return 2;
        }
        samples.push_back(
            std::chrono::duration<double, std::milli>(finish - start).count());
    }

    std::cout << "{\n"
              << "  \"schema\": \"zevryon.gpu-device-presentation-benchmark.v1\",\n"
              << "  \"input_document_lines\": " << kDocumentLines << ",\n"
              << "  \"input_projected_lines\": " << kProjectedLines << ",\n"
              << "  \"input_uploads\": " << kUploads << ",\n"
              << "  \"input_payload_bytes\": " << kPayloadBytes << ",\n"
              << "  \"input_draw_instances\": " << kDrawInstances << ",\n"
              << "  \"input_draw_batches\": " << kDrawBatches << ",\n"
              << "  \"selection_commands\": " << kSelectionCommands << ",\n"
              << "  \"caret_commands\": " << kCaretCommands << ",\n"
              << "  \"texture_count\": " << baseline.snapshot.texture_count << ",\n"
              << "  \"surface_image_count\": " << baseline.snapshot.surface_image_count << ",\n"
              << "  \"in_flight_frames\": " << baseline.snapshot.in_flight_frame_count << ",\n"
              << "  \"texture_pins\": " << baseline.snapshot.texture_pin_count << ",\n"
              << "  \"upload_submissions\": " << baseline.snapshot.upload_submissions << ",\n"
              << "  \"present_submissions\": " << baseline.snapshot.present_submissions << ",\n"
              << "  \"retired_frames\": " << baseline.snapshot.retired_frames << ",\n"
              << "  \"texture_allocations\": " << baseline.snapshot.texture_allocations << ",\n"
              << "  \"texture_reuses\": " << baseline.snapshot.texture_reuses << ",\n"
              << "  \"texture_evictions\": " << baseline.snapshot.texture_evictions << ",\n"
              << "  \"surface_reconfigurations\": " << baseline.snapshot.surface_reconfigurations << ",\n"
              << "  \"last_submitted_fence\": " << baseline.snapshot.last_submitted_fence_value << ",\n"
              << "  \"cold_present_fence\": 4,\n"
              << "  \"hot_present_fence\": " << baseline.receipt.signal_fence_value << ",\n"
              << "  \"frame_command_count\": " << baseline.receipt.command_count << ",\n"
              << "  \"metadata_current_bytes\": " << baseline.snapshot.metadata.current_bytes << ",\n"
              << "  \"metadata_peak_bytes\": " << baseline.snapshot.metadata.peak_bytes << ",\n"
              << "  \"metadata_hard_limit_bytes\": " << baseline.snapshot.metadata.hard_limit_bytes << ",\n"
              << "  \"texture_handle_bytes\": " << sizeof(GpuDeviceTextureHandle) << ",\n"
              << "  \"texture_record_bytes\": " << sizeof(GpuDeviceTextureRecord) << ",\n"
              << "  \"surface_image_handle_bytes\": " << sizeof(GpuSurfaceImageHandle) << ",\n"
              << "  \"surface_image_record_bytes\": " << sizeof(GpuSurfaceImageRecord) << ",\n"
              << "  \"texture_pin_bytes\": " << sizeof(GpuDeviceTexturePin) << ",\n"
              << "  \"present_receipt_bytes\": " << sizeof(GpuPresentReceipt) << ",\n"
              << "  \"in_flight_frame_bytes\": " << sizeof(GpuDeviceInFlightFrameRecord) << ",\n"
              << "  \"iterations\": " << iterations << ",\n"
              << "  \"p50_ms\": " << percentile(samples, 0.50) << ",\n"
              << "  \"p95_ms\": " << percentile(samples, 0.95) << ",\n"
              << "  \"p99_ms\": " << percentile(samples, 0.99) << ",\n"
              << "  \"maximum_ms\": " << *std::max_element(samples.begin(), samples.end()) << ",\n"
              << "  \"checksum\": " << baseline.checksum << "\n"
              << "}\n";
    return 0;
}
