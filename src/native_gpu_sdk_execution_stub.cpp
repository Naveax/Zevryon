#include "native_gpu_sdk_execution.hpp"

#include <utility>

namespace zevryon::text {
namespace {

class UnavailableNativeGpuSdkApi final : public NativeGpuSdkApi {
public:
    explicit UnavailableNativeGpuSdkApi(NativeGpuApiKind kind) noexcept
        : kind_(kind) {
        snapshot_.probe.api_kind = kind;
        snapshot_.probe.availability = NativeGpuSdkAvailability::Unavailable;
    }

    NativeGpuApiKind kind() const noexcept override { return kind_; }
    NativeGpuSdkProbe probe() noexcept override { return snapshot_.probe; }

    bool initialize(
        const NativeGpuSdkConfig&,
        NativeGpuSdkError* error) noexcept override {
        return unavailable(error, "requested native GPU SDK backend was not built");
    }

    bool configure_offscreen_surface(
        const GpuSurfaceDescriptor&,
        std::uint32_t,
        NativeGpuSdkError* error) noexcept override {
        return unavailable(error, "native GPU SDK backend is unavailable");
    }

    bool acquire_image(
        const GpuSurfaceDescriptor&,
        std::uint64_t,
        NativePlatformSwapchainImage*,
        NativeAcquireStatus*,
        NativeGpuSdkError* error) noexcept override {
        return unavailable(error, "native GPU SDK backend is unavailable");
    }

    bool execute_submission(
        const NativePlatformSubmission&,
        NativeGpuSdkSubmissionReceipt*,
        NativeGpuSdkError* error) noexcept override {
        return unavailable(error, "native GPU SDK backend is unavailable");
    }

    bool retire_completed(
        std::uint64_t,
        NativeGpuSdkError* error) noexcept override {
        return unavailable(error, "native GPU SDK backend is unavailable");
    }

    NativeGpuSdkSnapshot snapshot() const noexcept override { return snapshot_; }
    void shutdown() noexcept override {}

private:
    static bool unavailable(NativeGpuSdkError* error, const char* message) noexcept {
        if (error != nullptr) {
            error->kind = NativeGpuSdkErrorKind::UnsupportedBackend;
            error->native_code = 0;
            try {
                error->message = message;
            } catch (...) {
                error->message.clear();
            }
        }
        return false;
    }

    NativeGpuApiKind kind_{NativeGpuApiKind::ReferenceCpu};
    NativeGpuSdkSnapshot snapshot_;
};

std::unique_ptr<NativeGpuSdkApi> make_unavailable(NativeGpuApiKind kind) noexcept {
    try {
        return std::make_unique<UnavailableNativeGpuSdkApi>(kind);
    } catch (...) {
        return nullptr;
    }
}

} // namespace

#if !defined(ZEVRYON_HAS_VULKAN_SDK)
std::unique_ptr<NativeGpuSdkApi> make_vulkan_native_gpu_sdk_api() noexcept {
    return make_unavailable(NativeGpuApiKind::Vulkan);
}
#endif

#if !defined(ZEVRYON_HAS_METAL_SDK)
std::unique_ptr<NativeGpuSdkApi> make_metal_native_gpu_sdk_api() noexcept {
    return make_unavailable(NativeGpuApiKind::Metal);
}
#endif

#if !defined(ZEVRYON_HAS_D3D12_SDK)
std::unique_ptr<NativeGpuSdkApi> make_direct3d12_native_gpu_sdk_api() noexcept {
    return make_unavailable(NativeGpuApiKind::Direct3D12);
}
#endif

bool native_gpu_sdk_build_has_backend(NativeGpuApiKind kind) noexcept {
    switch (kind) {
        case NativeGpuApiKind::ReferenceCpu:
            return true;
        case NativeGpuApiKind::Vulkan:
#if defined(ZEVRYON_HAS_VULKAN_SDK)
            return true;
#else
            return false;
#endif
        case NativeGpuApiKind::Metal:
#if defined(ZEVRYON_HAS_METAL_SDK)
            return true;
#else
            return false;
#endif
        case NativeGpuApiKind::Direct3D12:
#if defined(ZEVRYON_HAS_D3D12_SDK)
            return true;
#else
            return false;
#endif
    }
    return false;
}

} // namespace zevryon::text
