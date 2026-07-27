#include "native_vulkan_wsi.hpp"

namespace zevryon::text {

#if !defined(ZEVRYON_HAS_VULKAN_WSI)
std::unique_ptr<NativeGpuSdkApi>
make_vulkan_wsi_native_gpu_sdk_api() noexcept {
    return nullptr;
}

std::unique_ptr<NativeWindowSwapchainApi>
make_vulkan_native_window_swapchain_api() noexcept {
    return nullptr;
}

bool native_vulkan_wsi_build_has_window_system(
    NativeWindowSystem) noexcept {
    return false;
}
#endif

} // namespace zevryon::text
