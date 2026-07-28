#pragma once

#include "native_window_swapchain.hpp"

#include <memory>

namespace zevryon::text {

// Creates the Z2F-8B2C Metal device owner. The owner exports one retained
// MTLDevice/MTLCommandQueue graph and never creates a parallel presentation
// device.
std::unique_ptr<NativeGpuSdkApi>
make_metal_window_native_gpu_sdk_api() noexcept;

// Creates the real CAMetalLayer presenter. Returns nullptr on non-Apple builds.
std::unique_ptr<NativeWindowSwapchainApi>
make_metal_native_window_swapchain_api() noexcept;

bool native_metal_window_build_has_backend(
    NativeWindowSystem system) noexcept;

} // namespace zevryon::text
