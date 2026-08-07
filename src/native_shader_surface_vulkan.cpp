#include "native_shader_surface_vulkan.hpp"

#if defined(ZEVRYON_HAS_VULKAN_WSI)

#include <array>
#include <cstring>

namespace zevryon::text::detail {
namespace {

VkImageMemoryBarrier image_barrier(
    VkImage image,
    VkImageLayout old_layout,
    VkImageLayout new_layout,
    VkAccessFlags source_access,
    VkAccessFlags destination_access) noexcept {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = source_access;
    barrier.dstAccessMask = destination_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0U;
    barrier.subresourceRange.levelCount = 1U;
    barrier.subresourceRange.baseArrayLayer = 0U;
    barrier.subresourceRange.layerCount = 1U;
    return barrier;
}

} // namespace

std::uint64_t vulkan_shader_surface_resource_id(VkImage image) noexcept {
    static_assert(sizeof(VkImage) <= sizeof(std::uint64_t));
    std::uint64_t id = 0U;
    std::memcpy(&id, &image, sizeof(VkImage));
    return id;
}

VkImage vulkan_shader_surface_resource_from_id(std::uint64_t id) noexcept {
    static_assert(sizeof(VkImage) <= sizeof(std::uint64_t));
    VkImage image = VK_NULL_HANDLE;
    std::memcpy(&image, &id, sizeof(VkImage));
    return image;
}

bool decode_vulkan_shader_surface(
    const NativeShaderSurfaceView& view,
    std::uint64_t expected_device_generation,
    std::uint64_t expected_runtime_generation,
    std::uint64_t expected_frame_id,
    std::uint64_t expected_content_checksum,
    std::uint32_t expected_width,
    std::uint32_t expected_height,
    VulkanShaderSurfaceSource* output) noexcept {
    if (output == nullptr) {
        return false;
    }
    *output = {};
    if (!native_shader_surface_view_valid(view) ||
        view.api_kind != NativeGpuApiKind::Vulkan ||
        view.device_generation != expected_device_generation ||
        view.runtime_generation != expected_runtime_generation ||
        view.frame_id != expected_frame_id ||
        view.content_checksum != expected_content_checksum ||
        view.width != expected_width || view.height != expected_height) {
        return false;
    }
    const VkImage image =
        vulkan_shader_surface_resource_from_id(view.native_resource);
    if (image == VK_NULL_HANDLE) {
        return false;
    }
    output->image = image;
    output->output_generation = view.output_generation;
    output->frame_id = view.frame_id;
    output->content_checksum = view.content_checksum;
    output->width = view.width;
    output->height = view.height;
    return true;
}

bool encode_vulkan_shader_surface_copy(
    VkCommandBuffer command_buffer,
    const VulkanShaderSurfaceSource& source,
    VkImage target,
    VkImageLayout* target_layout) noexcept {
    if (command_buffer == VK_NULL_HANDLE || source.image == VK_NULL_HANDLE ||
        target == VK_NULL_HANDLE || target_layout == nullptr ||
        source.width == 0U || source.height == 0U ||
        source.output_generation == 0U || source.frame_id == 0U ||
        source.content_checksum == 0U ||
        (*target_layout != VK_IMAGE_LAYOUT_UNDEFINED &&
         *target_layout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)) {
        return false;
    }

    const std::array<VkImageMemoryBarrier, 2U> to_copy{{
        image_barrier(
            source.image,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT),
        image_barrier(
            target,
            *target_layout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0U,
            VK_ACCESS_TRANSFER_WRITE_BIT)}};
    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0U,
        0U,
        nullptr,
        0U,
        nullptr,
        static_cast<std::uint32_t>(to_copy.size()),
        to_copy.data());

    VkImageCopy copy{};
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.mipLevel = 0U;
    copy.srcSubresource.baseArrayLayer = 0U;
    copy.srcSubresource.layerCount = 1U;
    copy.dstSubresource = copy.srcSubresource;
    copy.extent.width = source.width;
    copy.extent.height = source.height;
    copy.extent.depth = 1U;
    vkCmdCopyImage(
        command_buffer,
        source.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        target,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1U,
        &copy);

    const std::array<VkImageMemoryBarrier, 2U> after_copy{{
        image_barrier(
            source.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
        image_barrier(
            target,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            0U)}};
    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0U,
        0U,
        nullptr,
        0U,
        nullptr,
        static_cast<std::uint32_t>(after_copy.size()),
        after_copy.data());
    *target_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    return true;
}

} // namespace zevryon::text::detail

#endif
