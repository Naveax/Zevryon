#include "native_window_swapchain.hpp"

namespace zevryon::text {

#if !defined(ZEVRYON_HAS_D3D12_WINDOW_SWAPCHAIN)
std::unique_ptr<NativeWindowSwapchainApi>
make_direct3d12_native_window_swapchain_api() noexcept {
    return nullptr;
}
#endif

#if !defined(ZEVRYON_HAS_D3D12_WINDOW_SWAPCHAIN) && \
    !defined(ZEVRYON_HAS_METAL_WINDOW_SWAPCHAIN)
bool native_window_swapchain_build_has_backend(
    NativeGpuApiKind,
    NativeWindowSystem) noexcept {
    return false;
}
#endif

} // namespace zevryon::text
