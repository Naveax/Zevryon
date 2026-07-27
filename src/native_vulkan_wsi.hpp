#pragma once

#include "native_window_swapchain.hpp"

#include <memory>

namespace zevryon::text {

// Creates a Z2F-8A-compatible Vulkan device owner configured for a real
// Win32, XCB, or Wayland surface. The exported context is consumed by the
// Z2F-8B2B presenter without creating a second VkDevice or queue graph.
std::unique_ptr<NativeGpuSdkApi> make_vulkan_wsi_native_gpu_sdk_api() noexcept;

// Creates the real VkSwapchainKHR presenter. Returns nullptr when the build
// does not contain Vulkan WSI support.
std::unique_ptr<NativeWindowSwapchainApi>
make_vulkan_native_window_swapchain_api() noexcept;

bool native_vulkan_wsi_build_has_window_system(
    NativeWindowSystem system) noexcept;

} // namespace zevryon::text
