#pragma once

#include "native_shader_surface.hpp"

#if defined(ZEVRYON_HAS_VULKAN_WSI)

#include <vulkan/vulkan.h>

#include <cstdint>

namespace zevryon::text::detail {

struct VulkanShaderSurfaceSource final {
    VkImage image{VK_NULL_HANDLE};
    std::uint64_t output_generation{0U};
    std::uint64_t frame_id{0U};
    std::uint64_t content_checksum{0U};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
};

// Decode and validate the backend-neutral surface view before any native
// command is recorded. This deliberately binds the exported image to the same
// Vulkan device/runtime generation and the exact frame/checksum being
// presented.
bool decode_vulkan_shader_surface(
    const NativeShaderSurfaceView& view,
    std::uint64_t expected_device_generation,
    std::uint64_t expected_runtime_generation,
    std::uint64_t expected_frame_id,
    std::uint64_t expected_content_checksum,
    std::uint32_t expected_width,
    std::uint32_t expected_height,
    VulkanShaderSurfaceSource* output) noexcept;

// Record a raw 32-bit texel copy from the integer-composer output into one
// acquired BGRA8 swapchain image. R32_UINT and BGRA8 use the same four-byte
// texel block; vkCmdCopyImage preserves the canonical packed BGRA bytes without
// a CPU readback/upload hop or floating-point conversion.
//
// The source image is expected in GENERAL layout after shader execution. It is
// restored to GENERAL for later executor reuse. The target layout is updated
// to PRESENT_SRC_KHR only after all barriers and the copy have been recorded.
bool encode_vulkan_shader_surface_copy(
    VkCommandBuffer command_buffer,
    const VulkanShaderSurfaceSource& source,
    VkImage target,
    VkImageLayout* target_layout) noexcept;

std::uint64_t vulkan_shader_surface_resource_id(VkImage image) noexcept;
VkImage vulkan_shader_surface_resource_from_id(std::uint64_t id) noexcept;

} // namespace zevryon::text::detail

#endif
