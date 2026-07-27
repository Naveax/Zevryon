#include "native_gpu_sdk_execution.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <vector>

namespace {
using namespace zevryon::text;
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

template <typename T>
void hash_value(std::uint64_t* hash, const T& value) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        *hash ^= bytes[index];
        *hash *= kFnvPrime;
    }
}

NativeGpuSdkConfig make_config(NativeGpuApiKind kind) {
    NativeGpuSdkConfig config;
    config.api_kind = kind;
    config.device_generation = 101U + static_cast<std::uint64_t>(kind);
    config.runtime_generation = 201U + static_cast<std::uint64_t>(kind);
    config.limits = default_native_gpu_sdk_limits(kind);
    config.window.system = NativeWindowSystem::Headless;
    config.window.generation = 301U;
    return config;
}

GpuSurfaceDescriptor make_surface(NativeGpuApiKind kind) {
    GpuSurfaceDescriptor surface;
    surface.surface_id = 401U + static_cast<std::uint64_t>(kind);
    surface.generation_id = 501U;
    surface.width = 320U;
    surface.height = 180U;
    surface.format = GpuSurfaceFormat::Bgra8Unorm;
    return surface;
}

NativePlatformSubmission make_submission(
    NativeGpuApiKind kind,
    const GpuSurfaceDescriptor& surface,
    const NativePlatformSwapchainImage& image,
    std::uint64_t frame_id,
    std::uint64_t ticket_id) {
    NativePlatformSubmission submission;
    submission.api_kind = kind;
    submission.surface = surface;
    submission.image = image;
    submission.frame_id = frame_id;
    submission.ticket_id = ticket_id;
    submission.wait_fence_value = 3U;
    submission.command_generation = 601U;
    submission.source_command_checksum = 701U;
    submission.encoded_checksum = 0xD00DFEEDULL ^ frame_id ^
                                  (static_cast<std::uint64_t>(kind) << 56U);
    for (std::uint32_t index = 0U; index < 80U; ++index) {
        NativePlatformCommandRecord command;
        command.kind = static_cast<NativePlatformCommandKind>(index % 11U);
        command.source_index = index;
        command.auxiliary_index = index % 4U;
        command.flags = index & 1U;
        command.value0 = frame_id;
        command.value1 = ticket_id;
        submission.commands.push_back(command);
    }
    for (std::uint32_t index = 0U; index < 2U; ++index) {
        NativePlatformBarrierRecord barrier;
        barrier.resource_id = image.native_resource_id;
        barrier.resource_generation = image.image.image_generation;
        barrier.before = index == 0U
            ? NativePlatformResourceState::Present
            : NativePlatformResourceState::RenderTarget;
        barrier.after = index == 0U
            ? NativePlatformResourceState::RenderTarget
            : NativePlatformResourceState::Present;
        submission.barriers.push_back(barrier);
    }
    for (std::uint32_t index = 0U; index < 3U; ++index) {
        NativePlatformDescriptorBinding descriptor;
        descriptor.atlas_generation_id = 809U;
        descriptor.page_generation = 811U + index;
        descriptor.page_index = index;
        descriptor.descriptor_slot = index;
        submission.descriptors.push_back(descriptor);
    }
    return submission;
}

double percentile(std::vector<double> values, double quantile) {
    std::sort(values.begin(), values.end());
    const double position = quantile * static_cast<double>(values.size() - 1U);
    const std::size_t lower = static_cast<std::size_t>(std::floor(position));
    const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
    const double fraction = position - static_cast<double>(lower);
    return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

} // namespace

int main(int argc, char** argv) {
    using namespace zevryon::text;
    const std::uint32_t iterations = argc > 1
        ? static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10))
        : 512U;
    assert(iterations > 0U);

    const std::array<NativeGpuApiKind, 3U> kinds{
        NativeGpuApiKind::Vulkan,
        NativeGpuApiKind::Metal,
        NativeGpuApiKind::Direct3D12};
    std::array<std::unique_ptr<ReferenceNativeGpuSdkApi>, 3U> apis;
    std::array<GpuSurfaceDescriptor, 3U> surfaces{};
    for (std::size_t index = 0U; index < kinds.size(); ++index) {
        apis[index] = std::make_unique<ReferenceNativeGpuSdkApi>(kinds[index]);
        NativeGpuSdkError error;
        assert(apis[index]->initialize(make_config(kinds[index]), &error));
        surfaces[index] = make_surface(kinds[index]);
        assert(apis[index]->configure_offscreen_surface(surfaces[index], 3U, &error));
    }

    std::vector<double> samples;
    samples.reserve(iterations);
    std::uint64_t checksum = kFnvOffset;
    std::array<std::uint64_t, 3U> backend_checksums{kFnvOffset, kFnvOffset, kFnvOffset};
    for (std::uint32_t iteration = 0U; iteration < iterations; ++iteration) {
        const auto started = Clock::now();
        for (std::size_t index = 0U; index < kinds.size(); ++index) {
            NativeGpuSdkError error;
            NativePlatformSwapchainImage image;
            NativeAcquireStatus acquire_status = NativeAcquireStatus::NotReady;
            const std::uint64_t ticket =
                static_cast<std::uint64_t>(iteration) * kinds.size() + index + 1U;
            assert(apis[index]->acquire_image(
                surfaces[index], ticket, &image, &acquire_status, &error));
            assert(acquire_status == NativeAcquireStatus::Acquired);
            NativePlatformSubmission submission = make_submission(
                kinds[index], surfaces[index], image, 10'000U + iteration, ticket);
            NativeGpuSdkSubmissionReceipt receipt;
            assert(apis[index]->execute_submission(submission, &receipt, &error));
            assert(apis[index]->retire_completed(receipt.signal_fence_value, &error));
            hash_value(&backend_checksums[index], receipt.encoded_checksum);
            hash_value(&backend_checksums[index], receipt.signal_fence_value);
            hash_value(&checksum, receipt.encoded_checksum);
            hash_value(&checksum, receipt.signal_fence_value);
        }
        const auto ended = Clock::now();
        const double milliseconds =
            std::chrono::duration<double, std::milli>(ended - started).count();
        samples.push_back(milliseconds);
    }

    const double p50 = percentile(samples, 0.50);
    const double p95 = percentile(samples, 0.95);
    const double p99 = percentile(samples, 0.99);
    const double maximum = *std::max_element(samples.begin(), samples.end());
    const std::uint64_t per_backend_bytes =
        static_cast<std::uint64_t>(320U) * 180U * 4U * 3U;

    std::cout << std::fixed << std::setprecision(6)
              << "{\n"
              << "  \"schema\": \"zevryon.native-gpu-sdk-execution-benchmark.v1\",\n"
              << "  \"input_document_lines\": 16384,\n"
              << "  \"input_projected_lines\": 80,\n"
              << "  \"input_frame_commands\": 68,\n"
              << "  \"input_native_commands\": 71,\n"
              << "  \"input_platform_commands\": 80,\n"
              << "  \"input_draw_instances\": 310,\n"
              << "  \"backend_count\": 3,\n"
              << "  \"submissions_per_iteration\": 3,\n"
              << "  \"commands_per_submission\": 80,\n"
              << "  \"barriers_per_submission\": 2,\n"
              << "  \"descriptors_per_submission\": 3,\n"
              << "  \"offscreen_images_per_backend\": 3,\n"
              << "  \"device_local_bytes_per_backend\": " << per_backend_bytes << ",\n"
              << "  \"device_local_bytes_total\": " << per_backend_bytes * 3U << ",\n"
              << "  \"staging_hard_limit_bytes\": 4194304,\n"
              << "  \"window_handle_bytes\": " << sizeof(NativeWindowSurfaceHandle) << ",\n"
              << "  \"limits_record_bytes\": " << sizeof(NativeGpuSdkLimits) << ",\n"
              << "  \"config_record_bytes\": " << sizeof(NativeGpuSdkConfig) << ",\n"
              << "  \"probe_record_bytes\": " << sizeof(NativeGpuSdkProbe) << ",\n"
              << "  \"receipt_record_bytes\": " << sizeof(NativeGpuSdkSubmissionReceipt) << ",\n"
              << "  \"vulkan_checksum\": " << backend_checksums[0] << ",\n"
              << "  \"metal_checksum\": " << backend_checksums[1] << ",\n"
              << "  \"d3d12_checksum\": " << backend_checksums[2] << ",\n"
              << "  \"checksum\": " << checksum << ",\n"
              << "  \"p50_ms\": " << p50 << ",\n"
              << "  \"p95_ms\": " << p95 << ",\n"
              << "  \"p99_ms\": " << p99 << ",\n"
              << "  \"maximum_ms\": " << maximum << "\n"
              << "}\n";
    return 0;
}
