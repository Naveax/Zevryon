#include "native_gpu_sdk_execution.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <memory_resource>

namespace {
using namespace zevryon::text;

NativeGpuSdkConfig make_config(NativeGpuApiKind kind) {
    NativeGpuSdkConfig config;
    config.api_kind = kind;
    config.allow_software_device = 1U;
    config.require_real_device = 0U;
    config.enable_validation = 0U;
    config.device_generation = 7U;
    config.runtime_generation = 11U;
    config.limits = default_native_gpu_sdk_limits(kind);
    config.window.system = NativeWindowSystem::Headless;
    config.window.generation = 3U;
    return config;
}

NativePlatformAdapterConfig make_adapter_config(NativeGpuApiKind kind) {
    NativePlatformAdapterConfig config;
    config.api_kind = kind;
    config.maximum_commands = 256U;
    config.maximum_barriers = 64U;
    config.maximum_descriptors = 64U;
    config.maximum_swapchain_images = 3U;
    config.maximum_frames_in_flight = 2U;
    config.device_generation = 7U;
    config.driver_generation = 11U;
    return config;
}

GpuSurfaceDescriptor make_surface(std::uint64_t generation = 5U) {
    GpuSurfaceDescriptor surface;
    surface.surface_id = 101U;
    surface.generation_id = generation;
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
    submission.command_generation = 9U;
    submission.source_command_checksum = 1234U;
    submission.encoded_checksum = 5678U + frame_id;
    for (std::uint32_t index = 0U; index < 12U; ++index) {
        NativePlatformCommandRecord command;
        command.kind = static_cast<NativePlatformCommandKind>(
            index % (static_cast<std::uint32_t>(NativePlatformCommandKind::Present) + 1U));
        command.source_index = index;
        command.auxiliary_index = index % 3U;
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
        descriptor.atlas_generation_id = 13U;
        descriptor.page_generation = 17U + index;
        descriptor.page_index = index;
        descriptor.descriptor_slot = index;
        submission.descriptors.push_back(descriptor);
    }
    return submission;
}

void certify_reference_kind(NativeGpuApiKind kind) {
    ReferenceNativeGpuSdkApi api(kind);
    NativeGpuSdkConfig sdk_config = make_config(kind);
    NativeGpuSdkPlatformDriver driver(&api, sdk_config);
    const NativePlatformCapabilities capabilities = driver.capabilities();
    assert(capabilities.maximum_swapchain_images == 3U);
    assert((capabilities.flags & kNativePlatformPartialPresent) != 0U);

    NativeGpuApiError api_error;
    const GpuSurfaceDescriptor surface = make_surface();
    assert(driver.configure_swapchain(
        surface, 3U, make_adapter_config(kind), &api_error));

    NativePlatformSwapchainImage image;
    NativeAcquireStatus acquire_status = NativeAcquireStatus::NotReady;
    assert(driver.acquire_image(
        surface,
        NativePresentMode::Fifo,
        1U,
        &image,
        &acquire_status,
        &api_error));
    assert(acquire_status == NativeAcquireStatus::Acquired);
    assert(image.image.image_index == 0U);
    assert(image.driver_generation == 11U);

    NativePlatformSubmission submission = make_submission(kind, surface, image, 19U, 1U);
    std::uint64_t signal = 0U;
    std::uint64_t checksum = 0U;
    NativePresentStatus present_status = NativePresentStatus::SkippedNoDamage;
    assert(driver.submit_and_present(
        submission, &signal, &checksum, &present_status, &api_error));
    assert(signal == 1U);
    assert(checksum != 0U);
    assert(present_status == NativePresentStatus::Presented);

    NativeGpuSdkSnapshot snapshot = driver.snapshot();
    assert(snapshot.initialized_devices == 1U);
    assert(snapshot.configured_surfaces == 1U);
    assert(snapshot.acquired_images == 1U);
    assert(snapshot.submitted_frames == 1U);
    assert(snapshot.in_flight_frame_count == 1U);
    assert(snapshot.current_device_local_bytes ==
           static_cast<std::uint64_t>(surface.width) * surface.height * 4U * 3U);

    NativeGpuSdkError sdk_error;
    assert(driver.retire_completed(signal, &sdk_error));
    snapshot = driver.snapshot();
    assert(snapshot.retired_frames == 1U);
    assert(snapshot.in_flight_frame_count == 0U);
    assert(snapshot.completed_fence_value == signal);

    assert(!driver.retire_completed(0U, &sdk_error));
    assert(sdk_error.kind == NativeGpuSdkErrorKind::FenceRegression);

    NativePlatformSubmission stale = make_submission(
        kind, surface, image, 20U, 2U);
    stale.image.image.surface_generation += 1U;
    assert(!driver.submit_and_present(
        stale, &signal, &checksum, &present_status, &api_error));
    assert(api_error.kind == NativeGpuApiErrorKind::InvalidInput);

    api.set_next_acquire_status(NativeAcquireStatus::OutOfDate);
    assert(driver.acquire_image(
        surface,
        NativePresentMode::Fifo,
        2U,
        &image,
        &acquire_status,
        &api_error));
    assert(acquire_status == NativeAcquireStatus::OutOfDate);

    api.set_next_present_status(NativePresentStatus::DeviceLost);
    assert(driver.acquire_image(
        surface,
        NativePresentMode::Fifo,
        3U,
        &image,
        &acquire_status,
        &api_error));
    assert(acquire_status == NativeAcquireStatus::Acquired);
    NativePlatformSubmission lost_submission = make_submission(
        kind, surface, image, 21U, 3U);
    assert(!driver.submit_and_present(
        lost_submission, &signal, &checksum, &present_status, &api_error));
    assert(api_error.kind == NativeGpuApiErrorKind::DeviceLost);

    driver.shutdown();
    snapshot = driver.snapshot();
    assert(snapshot.configured_image_count == 0U);
    assert(snapshot.current_device_local_bytes == 0U);
}

void certify_budget_failure() {
    ReferenceNativeGpuSdkApi api(NativeGpuApiKind::Vulkan);
    NativeGpuSdkConfig config = make_config(NativeGpuApiKind::Vulkan);
    config.limits.maximum_device_local_bytes = 1024U;
    NativeGpuSdkPlatformDriver driver(&api, config);
    NativeGpuApiError error;
    assert(!driver.configure_swapchain(
        make_surface(), 3U, make_adapter_config(NativeGpuApiKind::Vulkan), &error));
    assert(error.kind == NativeGpuApiErrorKind::SurfaceConfigurationFailed);
    const NativeGpuSdkSnapshot snapshot = driver.snapshot();
    assert(snapshot.configured_surfaces == 0U);
    assert(snapshot.current_device_local_bytes == 0U);
}

void certify_initialization_failure() {
    ReferenceNativeGpuSdkApi api(NativeGpuApiKind::Vulkan);
    api.set_fail_initialization(true);
    NativeGpuSdkPlatformDriver driver(&api, make_config(NativeGpuApiKind::Vulkan));
    NativeGpuApiError error;
    assert(!driver.configure_swapchain(
        make_surface(), 3U, make_adapter_config(NativeGpuApiKind::Vulkan), &error));
    assert(error.kind == NativeGpuApiErrorKind::DeviceLost);
}

void certify_metal_unavailable_factory() {
    assert(!native_gpu_sdk_build_has_backend(NativeGpuApiKind::Metal));
    std::unique_ptr<NativeGpuSdkApi> api = make_metal_native_gpu_sdk_api();
    assert(api != nullptr);
    const NativeGpuSdkProbe probe = api->probe();
    assert(probe.api_kind == NativeGpuApiKind::Metal);
    assert(probe.availability == NativeGpuSdkAvailability::Unavailable);

    NativeGpuSdkError error;
    const NativeGpuSdkConfig config = make_config(NativeGpuApiKind::Metal);
    assert(!api->initialize(config, &error));
    assert(error.kind == NativeGpuSdkErrorKind::UnsupportedBackend);
}

} // namespace

int main() {
    certify_reference_kind(zevryon::text::NativeGpuApiKind::Vulkan);
    certify_reference_kind(zevryon::text::NativeGpuApiKind::Direct3D12);
    certify_budget_failure();
    certify_initialization_failure();
    certify_metal_unavailable_factory();
    std::cout << "native GPU SDK execution functional certification: PASS\n";
    return 0;
}
