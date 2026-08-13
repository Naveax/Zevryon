#include "native_gpu_sdk_execution.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>

namespace {
using namespace zevryon::text;

NativeGpuApiKind current_backend() noexcept {
#if defined(_WIN32)
    return NativeGpuApiKind::Direct3D12;
#else
    return NativeGpuApiKind::Vulkan;
#endif
}

std::unique_ptr<NativeGpuSdkApi> make_current_api() noexcept {
    switch (current_backend()) {
        case NativeGpuApiKind::Vulkan:
            return make_vulkan_native_gpu_sdk_api();
        case NativeGpuApiKind::Direct3D12:
            return make_direct3d12_native_gpu_sdk_api();
        case NativeGpuApiKind::Metal:
        case NativeGpuApiKind::ReferenceCpu:
            break;
    }
    return nullptr;
}

NativePlatformSubmission make_submission(
    NativeGpuApiKind kind,
    const GpuSurfaceDescriptor& surface,
    const NativePlatformSwapchainImage& image) {
    NativePlatformSubmission submission;
    submission.api_kind = kind;
    submission.surface = surface;
    submission.image = image;
    submission.frame_id = 1U;
    submission.ticket_id = 1U;
    submission.wait_fence_value = 0U;
    submission.command_generation = 1U;
    submission.source_command_checksum = 0x1234U;
    submission.encoded_checksum = 0xA1B2C3D4U;
    for (std::uint32_t index = 0U; index < 8U; ++index) {
        NativePlatformCommandRecord command;
        command.kind = static_cast<NativePlatformCommandKind>(index);
        command.source_index = index;
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
    NativePlatformDescriptorBinding descriptor;
    descriptor.atlas_generation_id = 2U;
    descriptor.page_generation = 3U;
    descriptor.page_index = 0U;
    descriptor.descriptor_slot = 0U;
    submission.descriptors.push_back(descriptor);
    return submission;
}

} // namespace

int main() {
    using namespace zevryon::text;
    const NativeGpuApiKind kind = current_backend();
    assert(native_gpu_sdk_build_has_backend(kind));
    std::unique_ptr<NativeGpuSdkApi> api = make_current_api();
    assert(api != nullptr);

    NativeGpuSdkConfig config;
    config.api_kind = kind;
    config.allow_software_device = 1U;
    config.require_real_device = 0U;
    config.enable_validation = 0U;
    config.device_generation = 101U;
    config.runtime_generation = 103U;
    config.limits = default_native_gpu_sdk_limits(kind);
    config.window.system = NativeWindowSystem::Headless;
    config.window.generation = 107U;

    NativeGpuSdkError error;
    if (!api->initialize(config, &error)) {
        std::cerr << "native backend initialization failed: "
                  << native_gpu_sdk_error_kind_name(error.kind)
                  << ": " << error.message << "\n";
        return 2;
    }
    NativeGpuSdkProbe probe = api->probe();
    assert(probe.api_kind == kind);
    assert(probe.availability == NativeGpuSdkAvailability::RuntimeReady);
    assert((probe.flags & kNativeGpuSdkRealDevice) != 0U);
    assert((probe.flags & kNativeGpuSdkOffscreenSurface) != 0U);

    GpuSurfaceDescriptor surface;
    surface.surface_id = 109U;
    surface.generation_id = 113U;
    surface.width = 64U;
    surface.height = 64U;
    surface.format = GpuSurfaceFormat::Bgra8Unorm;
    assert(api->configure_offscreen_surface(surface, 2U, &error));

    NativePlatformSwapchainImage image;
    NativeAcquireStatus acquire_status = NativeAcquireStatus::NotReady;
    assert(api->acquire_image(surface, 1U, &image, &acquire_status, &error));
    assert(acquire_status == NativeAcquireStatus::Acquired);

    NativePlatformSubmission submission = make_submission(kind, surface, image);
    NativeGpuSdkSubmissionReceipt receipt;
    if (!api->execute_submission(submission, &receipt, &error)) {
        std::cerr << "native backend execution failed: "
                  << native_gpu_sdk_error_kind_name(error.kind)
                  << ": " << error.message << "\n";
        return 3;
    }
    assert(receipt.status == NativePresentStatus::Presented);
    assert(receipt.command_count == 8U);
    assert(receipt.barrier_count == 2U);
    assert(receipt.descriptor_count == 1U);
    assert(receipt.signal_fence_value > 0U);
    assert(receipt.encoded_checksum != 0U);
    assert(api->retire_completed(receipt.signal_fence_value, &error));

    const NativeGpuSdkSnapshot snapshot = api->snapshot();
    assert(snapshot.initialized_devices == 1U);
    assert(snapshot.configured_surfaces == 1U);
    assert(snapshot.acquired_images == 1U);
    assert(snapshot.submitted_frames == 1U);
    assert(snapshot.retired_frames == 1U);
    assert(snapshot.current_device_local_bytes > 0U);
    api->shutdown();

    std::cout << "native GPU SDK platform smoke: api="
              << static_cast<unsigned>(kind)
              << " vendor=" << probe.vendor_id
              << " device=" << probe.device_id
              << " checksum=" << receipt.encoded_checksum
              << " PASS\n";
    return 0;
}
