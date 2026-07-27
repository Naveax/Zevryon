#pragma once

#include "native_gpu_sdk_execution.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>

#if defined(ZEVRYON_VULKAN_WSI_HAS_WIN32)
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#endif
#if defined(ZEVRYON_VULKAN_WSI_HAS_XCB)
#ifndef VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_XCB_KHR 1
#endif
#endif
#if defined(ZEVRYON_VULKAN_WSI_HAS_WAYLAND)
#ifndef VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR 1
#endif
#endif

#include <vulkan/vulkan.h>

namespace zevryon::text::detail {

constexpr std::uint32_t kNativeGpuSdkContextRetainedLease = 1U << 5U;
constexpr std::uint32_t kNativeGpuSdkContextVulkanWsi = 1U << 6U;

struct VulkanWsiSharedContext final {
    std::atomic<std::uint32_t> references{1U};
    std::mutex device_mutex;
    NativeWindowSurfaceHandle window;
    VkInstance instance{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue graphics_queue{VK_NULL_HANDLE};
    VkQueue present_queue{VK_NULL_HANDLE};
    VkSurfaceKHR surface{VK_NULL_HANDLE};
    std::uint32_t graphics_queue_family{0U};
    std::uint32_t present_queue_family{0U};
    std::uint64_t device_generation{0U};
    std::uint64_t runtime_generation{0U};
    std::atomic<std::uint8_t> owner_released{0U};
    std::uint8_t incremental_present{0U};
    std::uint8_t software_device{0U};
    std::uint8_t reserved{0U};
};

VulkanWsiSharedContext* retain_vulkan_wsi_context(
    const NativeGpuSdkContextHandle& context) noexcept;
void release_vulkan_wsi_context(VulkanWsiSharedContext* context) noexcept;
void release_vulkan_wsi_owner(VulkanWsiSharedContext* context) noexcept;

} // namespace zevryon::text::detail
