#include "native_window_swapchain.hpp"

namespace zevryon::text {

// Keep non-Windows builds link-complete without advertising a native WSI backend.
#if !defined(ZEVRYON_HAS_D3D12_WINDOW_SWAPCHAIN)
std::unique_ptr<NativeWindowSwapchainApi>
make_direct3d12_native_window_swapchain_api() noexcept {
    return nullptr;
}

bool native_window_swapchain_build_has_backend(
    NativeGpuApiKind,
    NativeWindowSystem) noexcept {
    return false;
}
#endif

} // namespace zevryon::text
