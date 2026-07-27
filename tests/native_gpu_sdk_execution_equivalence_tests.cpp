#include "native_gpu_sdk_execution.hpp"

#include <bit>
#include <cassert>
#include <cstdint>
#include <iostream>

namespace {
using namespace zevryon::text;

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

NativeGpuSdkConfig config_for(NativeGpuApiKind kind) {
    NativeGpuSdkConfig config;
    config.api_kind = kind;
    config.device_generation = 17U;
    config.runtime_generation = 23U;
    config.limits = default_native_gpu_sdk_limits(kind);
    config.limits.maximum_submission_commands = 64U;
    config.limits.maximum_descriptors = 8U;
    config.window.system = NativeWindowSystem::Headless;
    config.window.generation = 5U;
    return config;
}

GpuSurfaceDescriptor surface_for(std::uint64_t generation = 31U) {
    GpuSurfaceDescriptor surface;
    surface.surface_id = 29U;
    surface.generation_id = generation;
    surface.width = 64U;
    surface.height = 32U;
    return surface;
}

NativePlatformSubmission submission_for(
    NativeGpuApiKind kind,
    const GpuSurfaceDescriptor& surface,
    const NativePlatformSwapchainImage& image,
    std::uint32_t mask,
    std::uint32_t variant) {
    NativePlatformSubmission submission;
    submission.api_kind = kind;
    submission.surface = surface;
    submission.image = image;
    submission.frame_id = 1000U + mask;
    submission.ticket_id = 2000U + variant;
    submission.wait_fence_value = mask & 7U;
    submission.command_generation = 41U;
    submission.source_command_checksum = 43U;
    submission.encoded_checksum = 0xA5A50000ULL | mask;
    const std::uint32_t command_count = 8U + std::popcount(mask);
    for (std::uint32_t index = 0U; index < command_count; ++index) {
        NativePlatformCommandRecord command;
        command.kind = static_cast<NativePlatformCommandKind>(index % 11U);
        command.source_index = index;
        command.auxiliary_index = mask & 3U;
        command.flags = variant;
        command.value0 = submission.frame_id;
        command.value1 = submission.ticket_id;
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
    const std::uint32_t descriptor_count = 1U + (mask % 3U);
    for (std::uint32_t index = 0U; index < descriptor_count; ++index) {
        NativePlatformDescriptorBinding descriptor;
        descriptor.atlas_generation_id = 47U;
        descriptor.page_generation = 53U + index;
        descriptor.page_index = index;
        descriptor.descriptor_slot = index;
        submission.descriptors.push_back(descriptor);
    }
    return submission;
}

std::uint64_t expected_checksum(
    const NativePlatformSubmission& submission,
    NativeGpuApiKind kind) {
    std::uint64_t checksum = kFnvOffset;
    hash_value(&checksum, submission.encoded_checksum);
    hash_value(&checksum, submission.frame_id);
    hash_value(&checksum, submission.ticket_id);
    const std::size_t commands = submission.commands.size();
    const std::size_t barriers = submission.barriers.size();
    const std::size_t descriptors = submission.descriptors.size();
    hash_value(&checksum, commands);
    hash_value(&checksum, barriers);
    hash_value(&checksum, descriptors);
    hash_value(&checksum, kind);
    return checksum;
}

void run_case(NativeGpuApiKind kind, std::uint32_t mask, std::uint32_t variant) {
    ReferenceNativeGpuSdkApi api(kind);
    NativeGpuSdkError error;
    NativeGpuSdkConfig config = config_for(kind);
    const GpuSurfaceDescriptor surface = surface_for();
    assert(api.initialize(config, &error));
    assert(api.configure_offscreen_surface(surface, 3U, &error));

    NativePlatformSwapchainImage image;
    NativeAcquireStatus acquire_status = NativeAcquireStatus::NotReady;
    if (variant == 9U) {
        api.set_next_acquire_status(NativeAcquireStatus::OutOfDate);
    }
    assert(api.acquire_image(
        surface, 1U + mask, &image, &acquire_status, &error));
    if (variant == 9U) {
        assert(acquire_status == NativeAcquireStatus::OutOfDate);
        return;
    }
    assert(acquire_status == NativeAcquireStatus::Acquired);

    NativePlatformSubmission submission = submission_for(kind, surface, image, mask, variant);
    bool expected_success = true;
    NativeGpuSdkErrorKind expected_error = NativeGpuSdkErrorKind::None;
    NativePresentStatus expected_status = NativePresentStatus::Presented;

    switch (variant) {
        case 0U:
            break;
        case 1U:
            submission.image.image.device_generation += 1U;
            expected_success = false;
            expected_error = NativeGpuSdkErrorKind::StaleGeneration;
            break;
        case 2U:
            submission.image.driver_generation += 1U;
            expected_success = false;
            expected_error = NativeGpuSdkErrorKind::StaleGeneration;
            break;
        case 3U:
            submission.image.image.surface_generation += 1U;
            expected_success = false;
            expected_error = NativeGpuSdkErrorKind::StaleGeneration;
            break;
        case 4U:
            submission.api_kind = kind == NativeGpuApiKind::Vulkan
                ? NativeGpuApiKind::Metal
                : NativeGpuApiKind::Vulkan;
            expected_success = false;
            expected_error = NativeGpuSdkErrorKind::StaleGeneration;
            break;
        case 5U:
            while (submission.commands.size() <= config.limits.maximum_submission_commands) {
                submission.commands.push_back({});
            }
            expected_success = false;
            expected_error = NativeGpuSdkErrorKind::ResourceBudgetExceeded;
            break;
        case 6U:
            while (submission.descriptors.size() <= config.limits.maximum_descriptors) {
                submission.descriptors.push_back({});
            }
            expected_success = false;
            expected_error = NativeGpuSdkErrorKind::ResourceBudgetExceeded;
            break;
        case 7U:
            api.set_next_present_status(NativePresentStatus::DeviceLost);
            expected_success = false;
            expected_error = NativeGpuSdkErrorKind::DeviceLost;
            break;
        case 8U:
            api.set_next_present_status(NativePresentStatus::SkippedNoDamage);
            expected_status = NativePresentStatus::SkippedNoDamage;
            break;
        case 10U:
        case 11U:
            break;
        default:
            assert(false);
    }

    NativeGpuSdkSubmissionReceipt receipt;
    const bool success = api.execute_submission(submission, &receipt, &error);
    assert(success == expected_success);
    if (!success) {
        assert(error.kind == expected_error);
        return;
    }
    assert(receipt.api_kind == kind);
    assert(receipt.status == expected_status);
    assert(receipt.command_count == submission.commands.size());
    assert(receipt.barrier_count == submission.barriers.size());
    assert(receipt.descriptor_count == submission.descriptors.size());
    assert(receipt.signal_fence_value == 1U);
    assert(receipt.encoded_checksum == expected_checksum(submission, kind));

    if (variant == 10U) {
        assert(api.retire_completed(receipt.signal_fence_value, &error));
        assert(!api.retire_completed(0U, &error));
        assert(error.kind == NativeGpuSdkErrorKind::FenceRegression);
    } else if (variant == 11U) {
        assert(!api.retire_completed(receipt.signal_fence_value + 1U, &error));
        assert(error.kind == NativeGpuSdkErrorKind::FenceRegression);
    }
}

} // namespace

int main() {
    std::uint64_t cases = 0U;
    for (NativeGpuApiKind kind : {
             NativeGpuApiKind::Vulkan,
             NativeGpuApiKind::Metal,
             NativeGpuApiKind::Direct3D12}) {
        for (std::uint32_t mask = 0U; mask < 256U; ++mask) {
            for (std::uint32_t variant = 0U; variant < 12U; ++variant) {
                run_case(kind, mask, variant);
                ++cases;
            }
        }
    }
    assert(cases == 9216U);
    std::cout << "native GPU SDK execution oracle: " << cases << "/" << cases << " PASS\n";
    return 0;
}
