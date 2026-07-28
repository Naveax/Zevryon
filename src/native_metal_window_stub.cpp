#include "native_metal_window.hpp"

namespace zevryon::text {

#if !defined(ZEVRYON_HAS_METAL_WINDOW_SWAPCHAIN)
std::unique_ptr<NativeGpuSdkApi>
make_metal_window_native_gpu_sdk_api() noexcept {
    return nullptr;
}

std::unique_ptr<NativeWindowSwapchainApi>
make_metal_native_window_swapchain_api() noexcept {
    return nullptr;
}

bool native_metal_window_build_has_backend(
    NativeWindowSystem) noexcept {
    return false;
}
#endif

} // namespace zevryon::text
