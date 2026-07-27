#include "native_platform_adapters.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory_resource>
#include <vector>

using namespace zevryon::text;

namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void mix(std::uint64_t* hash, std::uint64_t value) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        *hash ^= (value >> shift) & 0xFFU;
        *hash *= kFnvPrime;
    }
}

NativePlatformAdapterConfig config_for(NativeGpuApiKind kind) {
    NativePlatformAdapterConfig config;
    config.api_kind = kind;
    config.maximum_commands = 256U;
    config.maximum_barriers = 16U;
    config.maximum_descriptors = 16U;
    config.maximum_swapchain_images = 3U;
    config.maximum_frames_in_flight = 2U;
    config.flags = kNativePlatformRequireTimelineFence |
        kNativePlatformAllowMailbox |
        kNativePlatformAllowImmediate |
        kNativePlatformAllowTearing;
    config.device_generation = 23U;
    config.driver_generation = 31U;
    return config;
}

struct Fixture final {
    std::array<std::byte, 512U * 1024U> storage{};
    std::pmr::monotonic_buffer_resource arena{
        storage.data(), storage.size(), std::pmr::null_memory_resource()};
    GpuFrameSubmission frame{&arena};
    NativeCommandBuffer commands{&arena};
    std::pmr::vector<GlyphAtlasDrawInstance> draws{&arena};

    Fixture() {
        frame.surface.surface_id = 301U;
        frame.surface.generation_id = 17U;
        frame.surface.width = 1920U;
        frame.surface.height = 1080U;
        frame.frame_id = 5001U;
        frame.atlas_generation_id = 41U;
        frame.required_upload_fence = 3U;

        for (std::uint32_t index = 0U; index < 65U; ++index) {
            TextPaintFillRect fill;
            fill.viewport_inline_start = static_cast<std::int64_t>((index % 13U) * 120U);
            fill.viewport_block_start = static_cast<std::int64_t>((index / 13U) * 36U);
            fill.inline_size = 110U;
            fill.block_size = 30U;
            fill.style_id = 100U + index;
            frame.fill_rects.push_back(fill);
        }

        for (std::uint32_t page_index = 0U; page_index < 3U; ++page_index) {
            GpuFramePageReference page;
            page.page_generation = 70U + page_index;
            page.required_upload_fence = page_index + 1U;
            page.page_index = page_index;
            page.first_batch = page_index;
            page.batch_count = 1U;
            page.format = page_index == 0U
                ? GlyphRasterFormat::Alpha8
                : (page_index == 1U ? GlyphRasterFormat::LcdRgb8 : GlyphRasterFormat::Bgra8);
            frame.page_references.push_back(page);

            GpuFrameGlyphBatch batch;
            batch.page_generation = page.page_generation;
            batch.page_index = page.page_index;
            batch.first_instance = page_index * 100U;
            batch.instance_count = page_index == 2U ? 110U : 100U;
            batch.style_id = 200U + page_index;
            batch.page_reference_index = page_index;
            frame.glyph_batches.push_back(batch);
        }

        for (std::uint32_t index = 0U; index < 310U; ++index) {
            const std::uint32_t page_index = index < 100U ? 0U : (index < 200U ? 1U : 2U);
            GlyphAtlasDrawInstance draw;
            draw.page_generation = 70U + page_index;
            draw.page_index = page_index;
            draw.width = 8U + page_index;
            draw.height = 12U + page_index;
            draw.style_id = 200U + page_index;
            draws.push_back(draw);
        }

        commands.surface = frame.surface;
        commands.frame_id = frame.frame_id;
        commands.command_generation = 9U;
        commands.command_checksum = 0xD00DFEED12345678ULL;
        commands.damage_rects.push_back({0, 0, 1920U, 1080U});
        commands.commands.push_back({NativeCommandKind::BeginRenderPass, 0U, 0U, 0U});
        commands.commands.push_back({NativeCommandKind::SetScissor, 0U, 0U, 0U});
        for (std::uint32_t index = 0U; index < 65U; ++index) {
            commands.commands.push_back({NativeCommandKind::FillRect, index, 0U, 0U});
        }
        for (std::uint32_t index = 0U; index < 3U; ++index) {
            commands.commands.push_back({NativeCommandKind::GlyphBatch, index, 0U, 0U});
        }
        commands.commands.push_back({NativeCommandKind::EndRenderPass, 0U, 0U, 0U});
        assert(commands.commands.size() == 71U);
    }
};

NativePlatformSwapchainImage image_for(
    const Fixture& fixture,
    NativeGpuApiKind kind) {
    NativePlatformSwapchainImage image;
    image.image.device_generation = 23U;
    image.image.surface_id = fixture.frame.surface.surface_id;
    image.image.surface_generation = fixture.frame.surface.generation_id;
    image.image.image_generation = 101U + static_cast<std::uint64_t>(kind);
    image.image.image_index = static_cast<std::uint32_t>(kind) - 1U;
    image.image.flags = 1U;
    image.driver_generation = 31U;
    image.native_resource_id = 1001U + static_cast<std::uint64_t>(kind);
    image.state = NativePlatformResourceState::Present;
    return image;
}

struct Distribution final {
    double p50{0.0};
    double p95{0.0};
    double p99{0.0};
    double maximum{0.0};
};

Distribution summarize(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    auto percentile = [&values](double p) {
        const std::size_t index = static_cast<std::size_t>(
            p * static_cast<double>(values.size() - 1U));
        return values[index];
    };
    return {percentile(0.50), percentile(0.95), percentile(0.99), values.back()};
}

} // namespace

int main(int argc, char** argv) {
    std::size_t iterations = 512U;
    if (argc > 1) {
        iterations = static_cast<std::size_t>(std::stoul(argv[1]));
    }
    Fixture fixture;
    const std::array<NativeGpuApiKind, 3U> kinds{
        NativeGpuApiKind::Vulkan,
        NativeGpuApiKind::Metal,
        NativeGpuApiKind::Direct3D12};

    std::uint64_t checksum = kFnvOffset;
    std::array<std::uint64_t, 3U> backend_checksums{};
    for (std::size_t backend = 0U; backend < kinds.size(); ++backend) {
        std::array<std::byte, 64U * 1024U> output_storage{};
        std::pmr::monotonic_buffer_resource output_arena{
            output_storage.data(), output_storage.size(), std::pmr::null_memory_resource()};
        NativePlatformSubmission output(&output_arena);
        NativePlatformCompileRequest request;
        request.commands = &fixture.commands;
        request.frame = &fixture.frame;
        request.draw_instances = fixture.draws;
        request.image = image_for(fixture, kinds[backend]);
        request.ticket_id = backend + 1U;
        request.wait_fence_value = 3U;
        request.config = config_for(kinds[backend]);
        NativePlatformCompileStats stats;
        NativePlatformCompileError error;
        assert(compile_native_platform_submission(request, &output, &stats, &error));
        assert(output.commands.size() == 80U);
        assert(output.barriers.size() == 2U);
        assert(output.descriptors.size() == 3U);
        assert(stats.fill_commands == 65U);
        assert(stats.glyph_draw_commands == 3U);
        assert(stats.scissor_commands == 1U);
        backend_checksums[backend] = output.encoded_checksum;
        mix(&checksum, output.encoded_checksum);
        mix(&checksum, output.commands.size());
        mix(&checksum, output.barriers.size());
        mix(&checksum, output.descriptors.size());
    }

    std::vector<double> samples;
    samples.reserve(iterations);
    for (std::size_t iteration = 0U; iteration < iterations; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t backend = 0U; backend < kinds.size(); ++backend) {
            std::array<std::byte, 64U * 1024U> output_storage{};
            std::pmr::monotonic_buffer_resource output_arena{
                output_storage.data(), output_storage.size(), std::pmr::null_memory_resource()};
            NativePlatformSubmission output(&output_arena);
            NativePlatformCompileRequest request;
            request.commands = &fixture.commands;
            request.frame = &fixture.frame;
            request.draw_instances = fixture.draws;
            request.image = image_for(fixture, kinds[backend]);
            request.ticket_id = backend + 1U;
            request.wait_fence_value = 3U;
            request.config = config_for(kinds[backend]);
            NativePlatformCompileError error;
            assert(compile_native_platform_submission(request, &output, nullptr, &error));
            assert(output.encoded_checksum == backend_checksums[backend]);
        }
        const auto stop = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
    }
    const Distribution distribution = summarize(std::move(samples));
    constexpr std::uint64_t logical_retained_per_backend =
        80U * sizeof(NativePlatformCommandRecord) +
        2U * sizeof(NativePlatformBarrierRecord) +
        3U * sizeof(NativePlatformDescriptorBinding);

    std::cout << std::fixed << std::setprecision(6)
              << "{\n"
              << "  \"schema\": \"zevryon.native-platform-adapters-benchmark.v1\",\n"
              << "  \"input_document_lines\": 16384,\n"
              << "  \"input_projected_lines\": 80,\n"
              << "  \"input_frame_commands\": 68,\n"
              << "  \"input_native_commands\": " << fixture.commands.commands.size() << ",\n"
              << "  \"input_draw_instances\": " << fixture.draws.size() << ",\n"
              << "  \"backend_count\": 3,\n"
              << "  \"commands_per_backend\": 80,\n"
              << "  \"barriers_per_backend\": 2,\n"
              << "  \"descriptors_per_backend\": 3,\n"
              << "  \"total_backend_commands\": 240,\n"
              << "  \"logical_retained_bytes_per_backend\": " << logical_retained_per_backend << ",\n"
              << "  \"logical_retained_bytes_total\": " << logical_retained_per_backend * 3U << ",\n"
              << "  \"adapter_scratch_hard_limit_bytes\": 262144,\n"
              << "  \"capability_record_bytes\": " << sizeof(NativePlatformCapabilities) << ",\n"
              << "  \"adapter_config_bytes\": " << sizeof(NativePlatformAdapterConfig) << ",\n"
              << "  \"command_record_bytes\": " << sizeof(NativePlatformCommandRecord) << ",\n"
              << "  \"barrier_record_bytes\": " << sizeof(NativePlatformBarrierRecord) << ",\n"
              << "  \"descriptor_record_bytes\": " << sizeof(NativePlatformDescriptorBinding) << ",\n"
              << "  \"swapchain_image_bytes\": " << sizeof(NativePlatformSwapchainImage) << ",\n"
              << "  \"vulkan_checksum\": " << backend_checksums[0] << ",\n"
              << "  \"metal_checksum\": " << backend_checksums[1] << ",\n"
              << "  \"d3d12_checksum\": " << backend_checksums[2] << ",\n"
              << "  \"checksum\": " << checksum << ",\n"
              << "  \"p50_ms\": " << distribution.p50 << ",\n"
              << "  \"p95_ms\": " << distribution.p95 << ",\n"
              << "  \"p99_ms\": " << distribution.p99 << ",\n"
              << "  \"maximum_ms\": " << distribution.maximum << "\n"
              << "}\n";
    return 0;
}
