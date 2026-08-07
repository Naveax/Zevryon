#include "native_vulkan_wsi.hpp"
#include "native_vulkan_wsi_context.hpp"
#include "native_shader_surface_vulkan.hpp"

#if defined(ZEVRYON_HAS_VULKAN_WSI)

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace zevryon::text {
namespace {

using detail::VulkanWsiSharedContext;
constexpr std::uint64_t kBytesPerPixel = 4U;
constexpr std::uint64_t kAcquireTimeoutNs = 5'000'000'000ULL;
constexpr std::uint64_t kFenceTimeoutNs = 5'000'000'000ULL;

void clear_error(NativeWindowSwapchainError* error) noexcept {
    if (error != nullptr) {
        error->kind = NativeWindowSwapchainErrorKind::None;
        error->native_code = 0;
        error->message.clear();
    }
}

bool fail(
    NativeWindowSwapchainError* error,
    NativeWindowSwapchainErrorKind kind,
    const char* message,
    VkResult result = VK_SUCCESS) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->native_code = static_cast<std::int64_t>(result);
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

template <typename T>
std::uint64_t opaque_handle_id(T handle) noexcept {
    static_assert(sizeof(T) <= sizeof(std::uint64_t));
    std::uint64_t output = 0U;
    std::memcpy(&output, &handle, sizeof(T));
    return output;
}

bool checked_surface_bytes(
    const GpuSurfaceDescriptor& surface,
    std::uint32_t image_count,
    std::uint64_t* bytes_per_image,
    std::uint64_t* total_bytes) noexcept {
    if (bytes_per_image == nullptr || total_bytes == nullptr ||
        surface.width == 0U || surface.height == 0U || image_count == 0U) {
        return false;
    }
    const std::uint64_t width = surface.width;
    const std::uint64_t height = surface.height;
    if (width > std::numeric_limits<std::uint64_t>::max() / height) {
        return false;
    }
    const std::uint64_t pixels = width * height;
    if (pixels > std::numeric_limits<std::uint64_t>::max() / kBytesPerPixel) {
        return false;
    }
    *bytes_per_image = pixels * kBytesPerPixel;
    if (*bytes_per_image >
        std::numeric_limits<std::uint64_t>::max() / image_count) {
        return false;
    }
    *total_bytes = *bytes_per_image * image_count;
    return true;
}

bool damage_rect_valid(
    const NativeDamageRect& rect,
    const GpuSurfaceDescriptor& surface) noexcept {
    if (rect.inline_start < 0 || rect.block_start < 0 ||
        rect.inline_size == 0U || rect.block_size == 0U) {
        return false;
    }
    const std::uint64_t x = static_cast<std::uint64_t>(rect.inline_start);
    const std::uint64_t y = static_cast<std::uint64_t>(rect.block_start);
    return x <= surface.width && y <= surface.height &&
        rect.inline_size <= static_cast<std::uint64_t>(surface.width) - x &&
        rect.block_size <= static_cast<std::uint64_t>(surface.height) - y;
}

VkFormat map_format(GpuSurfaceFormat format) noexcept {
    return format == GpuSurfaceFormat::Rgba8Unorm
        ? VK_FORMAT_R8G8B8A8_UNORM
        : VK_FORMAT_B8G8R8A8_UNORM;
}

bool present_mode_available(
    VkPresentModeKHR mode,
    const std::vector<VkPresentModeKHR>& available) noexcept {
    return std::find(available.begin(), available.end(), mode) != available.end();
}

struct VulkanHostTransfer final {
    VkDevice device{VK_NULL_HANDLE};
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};

    ~VulkanHostTransfer() {
        if (buffer != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, buffer, nullptr);
        }
        if (memory != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
            vkFreeMemory(device, memory, nullptr);
        }
    }
};

bool create_host_transfer(
    VulkanWsiSharedContext* context,
    const NativeWindowPixelBufferView& pixels,
    VulkanHostTransfer* output,
    NativeWindowSwapchainError* error) noexcept {
    if (context == nullptr || output == nullptr || pixels.empty()) {
        return false;
    }
    output->device = context->device;
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = pixels.bytes.size();
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = vkCreateBuffer(
        context->device, &buffer_info, nullptr, &output->buffer);
    if (result != VK_SUCCESS) {
        return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                    "Vulkan pixel staging buffer creation failed", result);
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(
        context->device, output->buffer, &requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(
        context->physical_device, &properties);
    std::uint32_t memory_type = properties.memoryTypeCount;
    for (std::uint32_t index = 0U;
         index < properties.memoryTypeCount; ++index) {
        const VkMemoryPropertyFlags flags =
            properties.memoryTypes[index].propertyFlags;
        if ((requirements.memoryTypeBits & (1U << index)) != 0U &&
            (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memory_type = index;
            break;
        }
    }
    if (memory_type == properties.memoryTypeCount) {
        return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                    "Vulkan exposes no coherent host-visible staging memory");
    }
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    result = vkAllocateMemory(
        context->device, &allocation, nullptr, &output->memory);
    if (result != VK_SUCCESS) {
        return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                    "Vulkan pixel staging memory allocation failed", result);
    }
    result = vkBindBufferMemory(
        context->device, output->buffer, output->memory, 0U);
    if (result != VK_SUCCESS) {
        return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                    "Vulkan pixel staging memory bind failed", result);
    }
    void* mapped = nullptr;
    result = vkMapMemory(
        context->device, output->memory, 0U, pixels.bytes.size(), 0U, &mapped);
    if (result != VK_SUCCESS || mapped == nullptr) {
        return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                    "Vulkan pixel staging memory map failed", result);
    }
    std::memcpy(mapped, pixels.bytes.data(), pixels.bytes.size());
    vkUnmapMemory(context->device, output->memory);
    return true;
}

class VulkanNativeWindowSwapchainApi final : public NativeWindowSwapchainApi {
public:
    VulkanNativeWindowSwapchainApi() noexcept {
#if defined(ZEVRYON_VULKAN_WSI_HAS_WIN32)
        constexpr NativeWindowSystem kDefaultWindowSystem =
            NativeWindowSystem::Win32;
#elif defined(ZEVRYON_VULKAN_WSI_HAS_XCB)
        constexpr NativeWindowSystem kDefaultWindowSystem =
            NativeWindowSystem::Xcb;
#else
        constexpr NativeWindowSystem kDefaultWindowSystem =
            NativeWindowSystem::Wayland;
#endif
        snapshot_.capabilities = default_native_window_swapchain_capabilities(
            NativeGpuApiKind::Vulkan,
            kDefaultWindowSystem);
    }

    ~VulkanNativeWindowSwapchainApi() override { shutdown(); }

    NativeWindowSwapchainCapabilities capabilities() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_.capabilities;
    }

    bool configure(
        const NativeWindowSwapchainConfig& config,
        NativeWindowSwapchainError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (snapshot_.configured != 0U) {
            return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                        "Vulkan window swapchain is already configured");
        }
        try {
            return configure_locked(config, false, error);
        } catch (const std::bad_alloc&) {
            return fail(error, NativeWindowSwapchainErrorKind::ResourceBudgetExceeded,
                        "Vulkan swapchain metadata allocation failed");
        } catch (...) {
            return fail(error, NativeWindowSwapchainErrorKind::ArithmeticOverflow,
                        "unexpected Vulkan swapchain configuration failure");
        }
    }

    bool request_resize(
        const GpuSurfaceDescriptor& surface,
        NativeWindowSwapchainError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (snapshot_.configured == 0U ||
            surface.surface_id != snapshot_.config.surface.surface_id ||
            surface.generation_id <= snapshot_.config.surface.generation_id ||
            surface.width == 0U || surface.height == 0U ||
            surface.width > snapshot_.config.limits.maximum_width ||
            surface.height > snapshot_.config.limits.maximum_height) {
            snapshot_.stale_rejections += 1U;
            return fail(error, NativeWindowSwapchainErrorKind::StaleGeneration,
                        "Vulkan resize does not advance a valid surface generation");
        }
        snapshot_.pending_surface = surface;
        snapshot_.out_of_date = 1U;
        snapshot_.resize_requests += 1U;
        snapshot_.out_of_date_events += 1U;
        return true;
    }

    bool recreate(
        const NativeWindowSwapchainConfig& config,
        NativeWindowSwapchainError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (snapshot_.configured == 0U || snapshot_.out_of_date == 0U ||
            snapshot_.pending_surface.surface_id == 0U) {
            return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                        "Vulkan swapchain recreation was not requested");
        }
        if (snapshot_.acquired_image_count != 0U ||
            snapshot_.in_flight_frame_count != 0U) {
            return fail(error, NativeWindowSwapchainErrorKind::Backpressure,
                        "Vulkan swapchain recreation requires a drained frame ring");
        }
        try {
            return configure_locked(config, true, error);
        } catch (const std::bad_alloc&) {
            return fail(error, NativeWindowSwapchainErrorKind::ResourceBudgetExceeded,
                        "Vulkan recreation metadata allocation failed");
        } catch (...) {
            return fail(error, NativeWindowSwapchainErrorKind::ArithmeticOverflow,
                        "unexpected Vulkan swapchain recreation failure");
        }
    }

    bool acquire(
        std::uint64_t ticket_id,
        NativeWindowSwapchainImage* image,
        NativeWindowAcquireStatus* status,
        NativeWindowSwapchainError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (image == nullptr || status == nullptr || ticket_id == 0U ||
            snapshot_.configured == 0U || context_ == nullptr ||
            swapchain_ == VK_NULL_HANDLE) {
            return fail(error, NativeWindowSwapchainErrorKind::AcquireFailed,
                        "invalid Vulkan window acquire request");
        }
        if (snapshot_.device_lost != 0U) {
            *status = NativeWindowAcquireStatus::DeviceLost;
            return true;
        }
        if (snapshot_.out_of_date != 0U) {
            *status = NativeWindowAcquireStatus::OutOfDate;
            return true;
        }
        if (snapshot_.acquired_image_count + snapshot_.in_flight_frame_count >=
            snapshot_.config.limits.maximum_frames_in_flight) {
            *status = NativeWindowAcquireStatus::NotReady;
            return true;
        }
        const std::uint32_t frame_index = find_free_frame_locked();
        if (frame_index == std::numeric_limits<std::uint32_t>::max()) {
            *status = NativeWindowAcquireStatus::NotReady;
            return true;
        }
        FrameSlot& frame = frames_[frame_index];
        std::uint32_t image_index = 0U;
        VkResult result;
        {
            std::lock_guard<std::mutex> device_lock(context_->device_mutex);
            result = vkAcquireNextImageKHR(
                context_->device,
                swapchain_,
                kAcquireTimeoutNs,
                frame.image_available,
                VK_NULL_HANDLE,
                &image_index);
        }
        if (result == VK_TIMEOUT || result == VK_NOT_READY) {
            *status = NativeWindowAcquireStatus::NotReady;
            return true;
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            snapshot_.out_of_date = 1U;
            snapshot_.out_of_date_events += 1U;
            *status = NativeWindowAcquireStatus::OutOfDate;
            return true;
        }
        if (result == VK_ERROR_DEVICE_LOST) {
            snapshot_.device_lost = 1U;
            snapshot_.device_lost_events += 1U;
            *status = NativeWindowAcquireStatus::DeviceLost;
            return true;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            return fail(error, NativeWindowSwapchainErrorKind::AcquireFailed,
                        "vkAcquireNextImageKHR failed", result);
        }
        if (image_index >= snapshot_.configured_image_count) {
            return fail(error, NativeWindowSwapchainErrorKind::AcquireFailed,
                        "Vulkan returned an out-of-range swapchain image index");
        }
        ImageSlot& slot = images_[image_index];
        if (slot.acquired != 0U) {
            return fail(error, NativeWindowSwapchainErrorKind::AcquireFailed,
                        "Vulkan returned an image with an active lease");
        }
        slot.acquired = 1U;
        slot.frame_index = frame_index;
        slot.image.acquire_serial = next_acquire_serial_++;
        slot.image.present_serial = 0U;
        slot.image.flags = kNativeWindowSwapchainImageAcquired;
        if (result == VK_SUBOPTIMAL_KHR) {
            slot.image.flags |= kNativeWindowSwapchainImageSuboptimal;
        }
        frame.image_index = image_index;
        frame.acquired = 1U;
        *image = slot.image;
        *status = result == VK_SUBOPTIMAL_KHR
            ? NativeWindowAcquireStatus::Suboptimal
            : NativeWindowAcquireStatus::Acquired;
        snapshot_.acquired_images += 1U;
        snapshot_.acquired_image_count += 1U;
        return true;
    }

    bool present(
        const NativeWindowPresentRequest& request,
        NativeWindowPresentReceipt* receipt,
        NativeWindowSwapchainError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (receipt == nullptr || snapshot_.configured == 0U ||
            context_ == nullptr || request.frame_id == 0U ||
            request.ticket_id == 0U ||
            request.image.image.image.image_index >=
                snapshot_.configured_image_count) {
            return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                        "invalid Vulkan window present request");
        }
        const std::uint32_t image_index = request.image.image.image.image_index;
        ImageSlot& slot = images_[image_index];
        if (slot.acquired == 0U ||
            request.image.swapchain_generation !=
                snapshot_.config.swapchain_generation ||
            request.image.acquire_serial != slot.image.acquire_serial ||
            request.image.image.image.device_generation !=
                snapshot_.config.context.device_generation ||
            request.image.image.driver_generation !=
                snapshot_.config.context.runtime_generation ||
            request.image.image.image.surface_id !=
                snapshot_.config.surface.surface_id ||
            request.image.image.image.surface_generation !=
                snapshot_.config.surface.generation_id ||
            request.image.image.image.image_generation !=
                slot.image.image.image.image_generation ||
            request.image.image.native_resource_id !=
                slot.image.image.native_resource_id ||
            slot.frame_index >= frames_.size()) {
            snapshot_.stale_rejections += 1U;
            return fail(error, NativeWindowSwapchainErrorKind::StaleGeneration,
                        "Vulkan present references a stale or unowned image");
        }
        if (request.damage_rects.size() >
            snapshot_.config.limits.maximum_damage_rects ||
            request.damage_rects.size() > present_rects_.size()) {
            return fail(error, NativeWindowSwapchainErrorKind::ResourceBudgetExceeded,
                        "Vulkan present region count exceeds the configured limit");
        }
        for (const NativeDamageRect& rect : request.damage_rects) {
            if (!damage_rect_valid(rect, snapshot_.config.surface)) {
                return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                            "Vulkan present rectangle is outside the surface");
            }
        }

        const bool has_pixel_buffer = !request.pixel_buffer.empty();
        const bool has_shader_surface = !request.shader_surface.empty();
        if (has_pixel_buffer && has_shader_surface) {
            return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                        "Vulkan present cannot use CPU and shader surfaces together");
        }
        if (has_pixel_buffer &&
            !native_window_pixel_buffer_valid(
                request.pixel_buffer, snapshot_.config.surface)) {
            return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                        "Vulkan pixel buffer does not match the surface");
        }

        detail::VulkanShaderSurfaceSource shader_surface_source{};
        if (has_shader_surface &&
            (swapchain_format_ != VK_FORMAT_B8G8R8A8_UNORM ||
             !detail::decode_vulkan_shader_surface(
                 request.shader_surface,
                 snapshot_.config.context.device_generation,
                 snapshot_.config.context.runtime_generation,
                 request.frame_id,
                 request.command_checksum,
                 snapshot_.config.surface.width,
                 snapshot_.config.surface.height,
                 &shader_surface_source))) {
            snapshot_.stale_rejections += 1U;
            return fail(error, NativeWindowSwapchainErrorKind::StaleGeneration,
                        "Vulkan shader surface is stale or incompatible");
        }
        if ((request.flags & kNativeWindowPresentAllowTearing) != 0U &&
            snapshot_.config.present_mode != NativePresentMode::Immediate) {
            return fail(error, NativeWindowSwapchainErrorKind::UnsupportedPresentMode,
                        "Vulkan tearing is only represented by immediate present mode");
        }
        if (snapshot_.in_flight_frame_count >=
            snapshot_.config.limits.maximum_frames_in_flight) {
            return fail(error, NativeWindowSwapchainErrorKind::Backpressure,
                        "Vulkan maximum frames in flight was reached");
        }

        FrameSlot& frame = frames_[slot.frame_index];
        if (next_fence_value_ == std::numeric_limits<std::uint64_t>::max()) {
            return fail(error, NativeWindowSwapchainErrorKind::FenceRegression,
                        "Vulkan window fence timeline exhausted");
        }
        if (frame.acquired == 0U || frame.in_flight != 0U) {
            snapshot_.stale_rejections += 1U;
            return fail(error, NativeWindowSwapchainErrorKind::StaleGeneration,
                        "Vulkan present frame slot is stale");
        }

        VkResult result;
        VkImageLayout recorded_layout = slot.layout;
        {
            std::lock_guard<std::mutex> device_lock(context_->device_mutex);
            result = vkResetFences(context_->device, 1U, &frame.fence);
            if (result != VK_SUCCESS) {
                return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                            "vkResetFences failed", result);
            }
            result = vkResetCommandPool(
                context_->device, frame.command_pool, 0U);
            if (result != VK_SUCCESS) {
                return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                            "vkResetCommandPool failed", result);
            }
            VulkanHostTransfer transfer;
            if (has_pixel_buffer &&
                !create_host_transfer(
                    context_, request.pixel_buffer, &transfer, error)) {
                return false;
            }
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            result = vkBeginCommandBuffer(frame.command_buffer, &begin);
            if (result != VK_SUCCESS) {
                return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                            "vkBeginCommandBuffer failed", result);
            }
            const bool render = !request.damage_rects.empty() ||
                (request.flags & kNativeWindowPresentFullRedraw) != 0U;
            if (render) {
                if (has_shader_surface) {
                    if (!detail::encode_vulkan_shader_surface_copy(
                            frame.command_buffer,
                            shader_surface_source,
                            slot.image_handle,
                            &recorded_layout)) {
                        return fail(
                            error,
                            NativeWindowSwapchainErrorKind::PresentFailed,
                            "Vulkan shader surface copy encoding failed");
                    }
                } else {
                    transition_image_locked(
                        frame.command_buffer,
                        slot.image_handle,
                        &recorded_layout,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                    if (has_pixel_buffer) {
                        VkBufferImageCopy copy{};
                        copy.bufferOffset = 0U;
                        copy.bufferRowLength = request.pixel_buffer.row_bytes / 4U;
                        copy.bufferImageHeight = request.pixel_buffer.height;
                        copy.imageSubresource.aspectMask =
                            VK_IMAGE_ASPECT_COLOR_BIT;
                        copy.imageSubresource.mipLevel = 0U;
                        copy.imageSubresource.baseArrayLayer = 0U;
                        copy.imageSubresource.layerCount = 1U;
                        copy.imageExtent.width = request.pixel_buffer.width;
                        copy.imageExtent.height = request.pixel_buffer.height;
                        copy.imageExtent.depth = 1U;
                        vkCmdCopyBufferToImage(
                            frame.command_buffer, transfer.buffer,
                            slot.image_handle,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &copy);
                    } else {
                        VkClearColorValue clear{};
                        clear.float32[0] = static_cast<float>(
                            (request.command_checksum >> 0U) & 0xFFU) / 255.0F;
                        clear.float32[1] = static_cast<float>(
                            (request.command_checksum >> 8U) & 0xFFU) / 255.0F;
                        clear.float32[2] = static_cast<float>(
                            (request.command_checksum >> 16U) & 0xFFU) / 255.0F;
                        clear.float32[3] = 1.0F;
                        VkImageSubresourceRange range{};
                        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                        range.levelCount = 1U;
                        range.layerCount = 1U;
                        vkCmdClearColorImage(
                            frame.command_buffer, slot.image_handle,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            &clear, 1U, &range);
                    }
                }
            }
            transition_image_locked(
                frame.command_buffer,
                slot.image_handle,
                &recorded_layout,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            result = vkEndCommandBuffer(frame.command_buffer);
            if (result != VK_SUCCESS) {
                return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                            "vkEndCommandBuffer failed", result);
            }
            const VkPipelineStageFlags wait_stage =
                VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.waitSemaphoreCount = 1U;
            submit.pWaitSemaphores = &frame.image_available;
            submit.pWaitDstStageMask = &wait_stage;
            submit.commandBufferCount = 1U;
            submit.pCommandBuffers = &frame.command_buffer;
            submit.signalSemaphoreCount = 1U;
            submit.pSignalSemaphores = &frame.render_finished;
            result = vkQueueSubmit(
                context_->graphics_queue, 1U, &submit, frame.fence);
            if (result != VK_SUCCESS) {
                if (result == VK_ERROR_DEVICE_LOST) {
                    snapshot_.device_lost = 1U;
                    snapshot_.device_lost_events += 1U;
                    return fail(error, NativeWindowSwapchainErrorKind::DeviceLost,
                                "Vulkan device was lost during queue submission",
                                result);
                }
                return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                            "vkQueueSubmit failed", result);
            }
            slot.layout = recorded_layout;
            if (has_pixel_buffer) {
                result = vkWaitForFences(
                    context_->device, 1U, &frame.fence,
                    VK_TRUE, kFenceTimeoutNs);
                if (result != VK_SUCCESS) {
                    return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                                "Vulkan pixel transfer fence wait failed", result);
                }
            }

            VkPresentInfoKHR present{};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.waitSemaphoreCount = 1U;
            present.pWaitSemaphores = &frame.render_finished;
            present.swapchainCount = 1U;
            present.pSwapchains = &swapchain_;
            present.pImageIndices = &image_index;
#if defined(VK_KHR_incremental_present)
            VkPresentRegionKHR region{};
            VkPresentRegionsKHR regions{};
            if (context_->incremental_present != 0U &&
                (snapshot_.config.flags &
                    kNativeWindowSwapchainAllowPartialPresent) != 0U &&
                !request.damage_rects.empty()) {
                for (std::size_t index = 0U;
                     index < request.damage_rects.size(); ++index) {
                    const NativeDamageRect& rect = request.damage_rects[index];
                    present_rects_[index].offset.x =
                        static_cast<std::int32_t>(rect.inline_start);
                    present_rects_[index].offset.y =
                        static_cast<std::int32_t>(rect.block_start);
                    present_rects_[index].extent.width =
                        static_cast<std::uint32_t>(rect.inline_size);
                    present_rects_[index].extent.height =
                        static_cast<std::uint32_t>(rect.block_size);
                    present_rects_[index].layer = 0U;
                }
                region.rectangleCount =
                    static_cast<std::uint32_t>(request.damage_rects.size());
                region.pRectangles = present_rects_.data();
                regions.sType = VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR;
                regions.swapchainCount = 1U;
                regions.pRegions = &region;
                present.pNext = &regions;
            }
#endif
            result = vkQueuePresentKHR(context_->present_queue, &present);
        }

        const std::uint64_t signal = next_fence_value_++;
        if (signal <= snapshot_.last_submitted_fence_value) {
            return fail(error, NativeWindowSwapchainErrorKind::FenceRegression,
                        "Vulkan window fence timeline regressed");
        }
        slot.acquired = 0U;
        slot.image.present_serial = next_present_serial_++;
        frame.acquired = 0U;
        frame.in_flight = 1U;
        frame.fence_value = signal;
        snapshot_.acquired_image_count -= 1U;
        snapshot_.in_flight_frame_count += 1U;
        snapshot_.current_in_flight_bytes += bytes_per_image_;
        snapshot_.peak_in_flight_bytes = std::max(
            snapshot_.peak_in_flight_bytes,
            snapshot_.current_in_flight_bytes);
        snapshot_.last_submitted_fence_value = signal;

        *receipt = {};
        receipt->image = request.image;
        receipt->image.present_serial = slot.image.present_serial;
        receipt->frame_id = request.frame_id;
        receipt->ticket_id = request.ticket_id;
        receipt->wait_fence_value = request.wait_fence_value;
        receipt->signal_fence_value = signal;
        receipt->command_checksum = request.command_checksum;
        receipt->command_count = request.command_count;
        receipt->damage_rect_count =
            static_cast<std::uint32_t>(request.damage_rects.size());

        if (result == VK_SUCCESS) {
            // Vulkan WSI cannot return an acquired image without presenting it;
            // even a no-damage frame consumes the acquire semaphore and calls
            // vkQueuePresentKHR.
            receipt->status = NativeWindowPresentStatus::Presented;
            snapshot_.presented_frames += 1U;
            return true;
        }
        if (result == VK_SUBOPTIMAL_KHR) {
            receipt->status = NativeWindowPresentStatus::Suboptimal;
            snapshot_.presented_frames += 1U;
            return true;
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            receipt->status = NativeWindowPresentStatus::OutOfDate;
            snapshot_.out_of_date = 1U;
            snapshot_.out_of_date_events += 1U;
            return true;
        }
        if (result == VK_ERROR_DEVICE_LOST) {
            receipt->status = NativeWindowPresentStatus::DeviceLost;
            snapshot_.device_lost = 1U;
            snapshot_.device_lost_events += 1U;
            return true;
        }
        return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                    "vkQueuePresentKHR failed", result);
    }

    bool retire_completed(
        std::uint64_t completed_fence_value,
        NativeWindowSwapchainError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (completed_fence_value < snapshot_.completed_fence_value ||
            completed_fence_value > snapshot_.last_submitted_fence_value) {
            return fail(error, NativeWindowSwapchainErrorKind::FenceRegression,
                        "Vulkan completion fence is outside the submitted timeline");
        }
        if (context_ == nullptr) {
            return fail(error, NativeWindowSwapchainErrorKind::NativeContextUnavailable,
                        "Vulkan native context is unavailable");
        }
        std::lock_guard<std::mutex> device_lock(context_->device_mutex);
        for (FrameSlot& frame : frames_) {
            if (frame.in_flight == 0U ||
                frame.fence_value > completed_fence_value) {
                continue;
            }
            const VkResult result = vkWaitForFences(
                context_->device, 1U, &frame.fence,
                VK_TRUE, kFenceTimeoutNs);
            if (result != VK_SUCCESS) {
                if (result == VK_ERROR_DEVICE_LOST) {
                    snapshot_.device_lost = 1U;
                    snapshot_.device_lost_events += 1U;
                    return fail(error, NativeWindowSwapchainErrorKind::DeviceLost,
                                "Vulkan device was lost while retiring a frame",
                                result);
                }
                return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                            "vkWaitForFences failed while retiring a frame",
                            result);
            }
            frame.in_flight = 0U;
            frame.fence_value = 0U;
            snapshot_.in_flight_frame_count -= 1U;
            snapshot_.current_in_flight_bytes -= bytes_per_image_;
        }
        snapshot_.completed_fence_value = completed_fence_value;
        return true;
    }

    NativeWindowSwapchainSnapshot snapshot() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

    void shutdown() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        destroy_swapchain_locked();
        if (context_ != nullptr) {
            detail::release_vulkan_wsi_context(context_);
            context_ = nullptr;
        }
        snapshot_.configured = 0U;
        snapshot_.configured_image_count = 0U;
        snapshot_.acquired_image_count = 0U;
        snapshot_.in_flight_frame_count = 0U;
        snapshot_.current_surface_bytes = 0U;
        snapshot_.current_in_flight_bytes = 0U;
    }

private:
    struct ImageSlot final {
        NativeWindowSwapchainImage image;
        VkImage image_handle{VK_NULL_HANDLE};
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
        std::uint32_t frame_index{0U};
        std::uint8_t acquired{0U};
        std::uint8_t reserved[3]{0U, 0U, 0U};
    };

    struct FrameSlot final {
        VkCommandPool command_pool{VK_NULL_HANDLE};
        VkCommandBuffer command_buffer{VK_NULL_HANDLE};
        VkSemaphore image_available{VK_NULL_HANDLE};
        VkSemaphore render_finished{VK_NULL_HANDLE};
        VkFence fence{VK_NULL_HANDLE};
        std::uint64_t fence_value{0U};
        std::uint32_t image_index{0U};
        std::uint8_t acquired{0U};
        std::uint8_t in_flight{0U};
        std::uint8_t reserved[2]{0U, 0U};
    };

    struct StagedResources final {
        VkSwapchainKHR swapchain{VK_NULL_HANDLE};
        std::array<VkImage, 16U> images{};
        std::array<FrameSlot, 16U> frames{};
        std::uint32_t image_count{0U};
        std::uint32_t frame_count{0U};
    };

    bool configure_locked(
        const NativeWindowSwapchainConfig& config,
        bool recreation,
        NativeWindowSwapchainError* error) {
        if (config.context.api_kind != NativeGpuApiKind::Vulkan ||
            config.context.device_generation == 0U ||
            config.context.runtime_generation == 0U ||
            config.swapchain_generation == 0U ||
            config.window.generation == 0U ||
            config.surface.surface_id == 0U ||
            config.surface.generation_id == 0U ||
            config.surface.width == 0U || config.surface.height == 0U ||
            config.image_count < 2U || config.image_count > 16U ||
            config.limits.maximum_frames_in_flight == 0U ||
            config.limits.maximum_frames_in_flight > frames_.size() ||
            config.limits.maximum_damage_rects > present_rects_.size()) {
            return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                        "invalid Vulkan window swapchain configuration");
        }
        if (recreation) {
            if (config.surface != snapshot_.pending_surface ||
                config.swapchain_generation <=
                    snapshot_.config.swapchain_generation ||
                config.context != snapshot_.config.context ||
                config.window != snapshot_.config.window) {
                snapshot_.stale_rejections += 1U;
                return fail(error, NativeWindowSwapchainErrorKind::StaleGeneration,
                            "Vulkan recreation does not advance the pending surface and swapchain generations");
            }
        }

        VulkanWsiSharedContext* retained = context_;
        std::unique_ptr<
            VulkanWsiSharedContext,
            void (*)(VulkanWsiSharedContext*)> retained_guard(
                nullptr,
                &detail::release_vulkan_wsi_context);
        if (!recreation) {
            retained = detail::retain_vulkan_wsi_context(config.context);
            if (retained == nullptr) {
                return fail(error, NativeWindowSwapchainErrorKind::NativeContextUnavailable,
                            "Vulkan native context lease is unavailable");
            }
            retained_guard.reset(retained);
        }
        if (retained->window != config.window ||
            retained->device_generation != config.context.device_generation ||
            retained->runtime_generation != config.context.runtime_generation) {
            return fail(error, NativeWindowSwapchainErrorKind::StaleGeneration,
                        "Vulkan window or native context generation is stale");
        }

        std::lock_guard<std::mutex> device_lock(retained->device_mutex);
        VkSurfaceCapabilitiesKHR surface_capabilities{};
        VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            retained->physical_device,
            retained->surface,
            &surface_capabilities);
        if (result != VK_SUCCESS) {
            return fail(error, NativeWindowSwapchainErrorKind::SurfaceCreationFailed,
                        "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed",
                        result);
        }
        std::uint32_t format_count = 0U;
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(
            retained->physical_device,
            retained->surface,
            &format_count,
            nullptr);
        if (result != VK_SUCCESS || format_count == 0U) {
            return fail(error, NativeWindowSwapchainErrorKind::SurfaceCreationFailed,
                        "Vulkan surface exposes no formats", result);
        }
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(
            retained->physical_device,
            retained->surface,
            &format_count,
            formats.data());
        if (result != VK_SUCCESS) {
            return fail(error, NativeWindowSwapchainErrorKind::SurfaceCreationFailed,
                        "vkGetPhysicalDeviceSurfaceFormatsKHR failed", result);
        }
        std::uint32_t present_mode_count = 0U;
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(
            retained->physical_device,
            retained->surface,
            &present_mode_count,
            nullptr);
        if (result != VK_SUCCESS || present_mode_count == 0U) {
            return fail(error, NativeWindowSwapchainErrorKind::UnsupportedPresentMode,
                        "Vulkan surface exposes no present modes", result);
        }
        std::vector<VkPresentModeKHR> present_modes(present_mode_count);
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(
            retained->physical_device,
            retained->surface,
            &present_mode_count,
            present_modes.data());
        if (result != VK_SUCCESS) {
            return fail(error, NativeWindowSwapchainErrorKind::UnsupportedPresentMode,
                        "vkGetPhysicalDeviceSurfacePresentModesKHR failed", result);
        }

        VkPresentModeKHR selected_present_mode = VK_PRESENT_MODE_FIFO_KHR;
        if (config.present_mode == NativePresentMode::Mailbox) {
            if (!present_mode_available(VK_PRESENT_MODE_MAILBOX_KHR, present_modes) ||
                (config.flags & kNativeWindowSwapchainAllowMailbox) == 0U) {
                if (!recreation) {
                    detail::release_vulkan_wsi_context(retained);
                }
                return fail(error, NativeWindowSwapchainErrorKind::UnsupportedPresentMode,
                            "Vulkan mailbox present mode is unavailable");
            }
            selected_present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
        } else if (config.present_mode == NativePresentMode::Immediate) {
            if (!present_mode_available(VK_PRESENT_MODE_IMMEDIATE_KHR, present_modes) ||
                (config.flags & kNativeWindowSwapchainAllowImmediate) == 0U) {
                if (!recreation) {
                    detail::release_vulkan_wsi_context(retained);
                }
                return fail(error, NativeWindowSwapchainErrorKind::UnsupportedPresentMode,
                            "Vulkan immediate present mode is unavailable");
            }
            selected_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }

        const VkFormat requested_format = map_format(config.surface.format);
        VkSurfaceFormatKHR selected_format = formats.front();
        for (const auto& candidate : formats) {
            if (candidate.format == requested_format &&
                candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                selected_format = candidate;
                break;
            }
        }
        VkExtent2D extent{};
        if (surface_capabilities.currentExtent.width !=
            std::numeric_limits<std::uint32_t>::max()) {
            extent = surface_capabilities.currentExtent;
        } else {
            extent.width = std::clamp(
                config.surface.width,
                surface_capabilities.minImageExtent.width,
                surface_capabilities.maxImageExtent.width);
            extent.height = std::clamp(
                config.surface.height,
                surface_capabilities.minImageExtent.height,
                surface_capabilities.maxImageExtent.height);
        }
        if (extent.width != config.surface.width ||
            extent.height != config.surface.height) {
            return fail(error, NativeWindowSwapchainErrorKind::OutOfDate,
                        "Vulkan compositor surface extent differs from the requested generation");
        }
        if ((surface_capabilities.supportedUsageFlags &
             VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0U) {
            return fail(error, NativeWindowSwapchainErrorKind::SwapchainCreationFailed,
                        "Vulkan surface does not support transfer-destination back buffers");
        }

        std::uint32_t requested_images = std::max<std::uint32_t>(
            config.image_count,
            surface_capabilities.minImageCount);
        if (surface_capabilities.maxImageCount != 0U) {
            requested_images = std::min(
                requested_images,
                surface_capabilities.maxImageCount);
        }
        requested_images = std::min(
            requested_images,
            config.limits.maximum_image_count);
        if (requested_images < surface_capabilities.minImageCount ||
            requested_images > images_.size()) {
            return fail(error, NativeWindowSwapchainErrorKind::ResourceBudgetExceeded,
                        "Vulkan swapchain image count exceeds bounded limits");
        }

        StagedResources staged;
        staged.frame_count = config.limits.maximum_frames_in_flight;
        if (!create_frame_resources_locked(retained, &staged, error)) {
            destroy_staged_locked(retained, &staged);
            return false;
        }
        const std::array<std::uint32_t, 2U> queue_families{
            retained->graphics_queue_family,
            retained->present_queue_family};
        VkSwapchainCreateInfoKHR create{};
        create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        create.surface = retained->surface;
        create.minImageCount = requested_images;
        create.imageFormat = selected_format.format;
        create.imageColorSpace = selected_format.colorSpace;
        create.imageExtent = extent;
        create.imageArrayLayers = 1U;
        create.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (retained->graphics_queue_family != retained->present_queue_family) {
            create.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            create.queueFamilyIndexCount = 2U;
            create.pQueueFamilyIndices = queue_families.data();
        } else {
            create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }
        create.preTransform =
            (surface_capabilities.supportedTransforms &
             VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0U
            ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
            : surface_capabilities.currentTransform;
        create.compositeAlpha = choose_composite_alpha(
            surface_capabilities.supportedCompositeAlpha);
        create.presentMode = selected_present_mode;
        create.clipped = VK_TRUE;
        create.oldSwapchain = recreation ? swapchain_ : VK_NULL_HANDLE;
        result = vkCreateSwapchainKHR(
            retained->device, &create, nullptr, &staged.swapchain);
        if (result != VK_SUCCESS) {
            destroy_staged_locked(retained, &staged);
            return fail(error, NativeWindowSwapchainErrorKind::SwapchainCreationFailed,
                        "vkCreateSwapchainKHR failed", result);
        }
        std::uint32_t actual_image_count = 0U;
        result = vkGetSwapchainImagesKHR(
            retained->device,
            staged.swapchain,
            &actual_image_count,
            nullptr);
        if (result != VK_SUCCESS || actual_image_count == 0U ||
            actual_image_count > staged.images.size() ||
            actual_image_count > config.limits.maximum_image_count) {
            destroy_staged_locked(retained, &staged);
            return fail(error, NativeWindowSwapchainErrorKind::SwapchainCreationFailed,
                        "Vulkan swapchain returned an invalid image count", result);
        }
        result = vkGetSwapchainImagesKHR(
            retained->device,
            staged.swapchain,
            &actual_image_count,
            staged.images.data());
        if (result != VK_SUCCESS) {
            destroy_staged_locked(retained, &staged);
            return fail(error, NativeWindowSwapchainErrorKind::SwapchainCreationFailed,
                        "vkGetSwapchainImagesKHR failed", result);
        }
        staged.image_count = actual_image_count;

        GpuSurfaceDescriptor actual_surface = config.surface;
        actual_surface.width = extent.width;
        actual_surface.height = extent.height;
        std::uint64_t bytes_per_image = 0U;
        std::uint64_t total_bytes = 0U;
        if (!checked_surface_bytes(
                actual_surface,
                actual_image_count,
                &bytes_per_image,
                &total_bytes) ||
            total_bytes > config.limits.maximum_surface_bytes ||
            bytes_per_image >
                config.limits.maximum_in_flight_bytes /
                    config.limits.maximum_frames_in_flight) {
            destroy_staged_locked(retained, &staged);
            return fail(error, NativeWindowSwapchainErrorKind::ResourceBudgetExceeded,
                        "Vulkan swapchain exceeds configured surface or in-flight budget");
        }

        if (recreation) {
            destroy_swapchain_resources_no_lock(retained, false);
        } else {
            context_ = retained;
            (void)retained_guard.release();
        }
        swapchain_ = staged.swapchain;
        staged.swapchain = VK_NULL_HANDLE;
        swapchain_format_ = selected_format.format;
        bytes_per_image_ = bytes_per_image;
        for (std::uint32_t index = 0U; index < actual_image_count; ++index) {
            images_[index] = {};
            images_[index].image_handle = staged.images[index];
            images_[index].layout = VK_IMAGE_LAYOUT_UNDEFINED;
            images_[index].image.image.image.device_generation =
                config.context.device_generation;
            images_[index].image.image.image.surface_id = config.surface.surface_id;
            images_[index].image.image.image.surface_generation =
                config.surface.generation_id;
            images_[index].image.image.image.image_generation =
                next_image_generation_++;
            images_[index].image.image.image.image_index = index;
            images_[index].image.image.driver_generation =
                config.context.runtime_generation;
            images_[index].image.image.native_resource_id =
                opaque_handle_id(staged.images[index]);
            images_[index].image.image.state =
                NativePlatformResourceState::Present;
            images_[index].image.swapchain_generation =
                config.swapchain_generation;
        }
        for (std::uint32_t index = 0U; index < staged.frame_count; ++index) {
            frames_[index] = staged.frames[index];
            staged.frames[index] = {};
        }
        snapshot_.config = config;
        snapshot_.config.surface = actual_surface;
        snapshot_.pending_surface = {};
        snapshot_.configured = 1U;
        snapshot_.out_of_date = 0U;
        snapshot_.occluded = 0U;
        snapshot_.device_lost = 0U;
        snapshot_.configured_image_count = actual_image_count;
        snapshot_.acquired_image_count = 0U;
        snapshot_.in_flight_frame_count = 0U;
        snapshot_.current_surface_bytes = total_bytes;
        snapshot_.peak_surface_bytes = std::max(
            snapshot_.peak_surface_bytes,
            total_bytes);
        snapshot_.current_in_flight_bytes = 0U;
        snapshot_.configurations += recreation ? 0U : 1U;
        snapshot_.recreations += recreation ? 1U : 0U;
        update_capabilities_locked(
            surface_capabilities,
            present_modes,
            retained,
            actual_image_count);
        destroy_staged_locked(retained, &staged);
        return true;
    }

    bool create_frame_resources_locked(
        VulkanWsiSharedContext* context,
        StagedResources* staged,
        NativeWindowSwapchainError* error) noexcept {
        for (std::uint32_t index = 0U; index < staged->frame_count; ++index) {
            FrameSlot& frame = staged->frames[index];
            VkCommandPoolCreateInfo pool{};
            pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                         VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pool.queueFamilyIndex = context->graphics_queue_family;
            VkResult result = vkCreateCommandPool(
                context->device, &pool, nullptr, &frame.command_pool);
            if (result != VK_SUCCESS) {
                return fail(error, NativeWindowSwapchainErrorKind::SwapchainCreationFailed,
                            "vkCreateCommandPool for window frame failed", result);
            }
            VkCommandBufferAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool = frame.command_pool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1U;
            result = vkAllocateCommandBuffers(
                context->device, &allocate, &frame.command_buffer);
            if (result != VK_SUCCESS) {
                return fail(error, NativeWindowSwapchainErrorKind::SwapchainCreationFailed,
                            "vkAllocateCommandBuffers for window frame failed", result);
            }
            VkSemaphoreCreateInfo semaphore{};
            semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            result = vkCreateSemaphore(
                context->device, &semaphore, nullptr, &frame.image_available);
            if (result != VK_SUCCESS) {
                return fail(error, NativeWindowSwapchainErrorKind::SwapchainCreationFailed,
                            "vkCreateSemaphore for image acquire failed", result);
            }
            result = vkCreateSemaphore(
                context->device, &semaphore, nullptr, &frame.render_finished);
            if (result != VK_SUCCESS) {
                return fail(error, NativeWindowSwapchainErrorKind::SwapchainCreationFailed,
                            "vkCreateSemaphore for render completion failed", result);
            }
            VkFenceCreateInfo fence{};
            fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            result = vkCreateFence(
                context->device, &fence, nullptr, &frame.fence);
            if (result != VK_SUCCESS) {
                return fail(error, NativeWindowSwapchainErrorKind::SwapchainCreationFailed,
                            "vkCreateFence for window frame failed", result);
            }
        }
        return true;
    }

    void transition_image_locked(
        VkCommandBuffer command_buffer,
        VkImage image,
        VkImageLayout* current_layout,
        VkImageLayout target) noexcept {
        if (current_layout == nullptr || *current_layout == target) {
            return;
        }
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = *current_layout;
        barrier.newLayout = target;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1U;
        barrier.subresourceRange.layerCount = 1U;
        VkPipelineStageFlags source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags destination_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        if (target == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            if (*current_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
                source_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            }
        } else if (target == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
            if (*current_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            }
            destination_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        }
        vkCmdPipelineBarrier(
            command_buffer,
            source_stage,
            destination_stage,
            0U,
            0U,
            nullptr,
            0U,
            nullptr,
            1U,
            &barrier);
        *current_layout = target;
    }

    std::uint32_t find_free_frame_locked() const noexcept {
        const std::uint32_t count =
            snapshot_.config.limits.maximum_frames_in_flight;
        for (std::uint32_t index = 0U; index < count; ++index) {
            if (frames_[index].acquired == 0U &&
                frames_[index].in_flight == 0U) {
                return index;
            }
        }
        return std::numeric_limits<std::uint32_t>::max();
    }

    static VkCompositeAlphaFlagBitsKHR choose_composite_alpha(
        VkCompositeAlphaFlagsKHR supported) noexcept {
        constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4U> choices{
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
        for (const auto choice : choices) {
            if ((supported & choice) != 0U) {
                return choice;
            }
        }
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }

    void update_capabilities_locked(
        const VkSurfaceCapabilitiesKHR& surface,
        const std::vector<VkPresentModeKHR>& modes,
        VulkanWsiSharedContext* context,
        std::uint32_t actual_image_count) noexcept {
        snapshot_.capabilities = {};
        snapshot_.capabilities.flags =
            kNativeWindowSwapchainWindowSurface |
            kNativeWindowSwapchainResize;
        if (present_mode_available(VK_PRESENT_MODE_MAILBOX_KHR, modes)) {
            snapshot_.capabilities.flags |= kNativeWindowSwapchainMailbox;
        }
        if (present_mode_available(VK_PRESENT_MODE_IMMEDIATE_KHR, modes)) {
            snapshot_.capabilities.flags |=
                kNativeWindowSwapchainImmediate |
                kNativeWindowSwapchainTearing;
        }
        if (context->incremental_present != 0U) {
            snapshot_.capabilities.flags |=
                kNativeWindowSwapchainPartialPresent;
        }
        if (context->graphics_queue_family != context->present_queue_family) {
            snapshot_.capabilities.flags |=
                kNativeWindowSwapchainSeparatePresentQueue;
        }
        snapshot_.capabilities.minimum_image_count = surface.minImageCount;
        snapshot_.capabilities.maximum_image_count =
            surface.maxImageCount == 0U
            ? static_cast<std::uint32_t>(images_.size())
            : std::min<std::uint32_t>(
                surface.maxImageCount,
                static_cast<std::uint32_t>(images_.size()));
        snapshot_.capabilities.maximum_frames_in_flight =
            snapshot_.config.limits.maximum_frames_in_flight;
        snapshot_.capabilities.maximum_damage_rects =
            snapshot_.config.limits.maximum_damage_rects;
        snapshot_.capabilities.maximum_width =
            surface.maxImageExtent.width;
        snapshot_.capabilities.maximum_height =
            surface.maxImageExtent.height;
        snapshot_.capabilities.maximum_surface_bytes =
            snapshot_.config.limits.maximum_surface_bytes;
        (void)actual_image_count;
    }

    void destroy_staged_locked(
        VulkanWsiSharedContext* context,
        StagedResources* staged) noexcept {
        if (context == nullptr || staged == nullptr) {
            return;
        }
        for (FrameSlot& frame : staged->frames) {
            if (frame.fence != VK_NULL_HANDLE) {
                vkDestroyFence(context->device, frame.fence, nullptr);
            }
            if (frame.render_finished != VK_NULL_HANDLE) {
                vkDestroySemaphore(
                    context->device, frame.render_finished, nullptr);
            }
            if (frame.image_available != VK_NULL_HANDLE) {
                vkDestroySemaphore(
                    context->device, frame.image_available, nullptr);
            }
            if (frame.command_pool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(
                    context->device, frame.command_pool, nullptr);
            }
            frame = {};
        }
        if (staged->swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(context->device, staged->swapchain, nullptr);
            staged->swapchain = VK_NULL_HANDLE;
        }
    }

    void destroy_swapchain_resources_no_lock(
        VulkanWsiSharedContext* context,
        bool wait_idle) noexcept {
        if (context == nullptr) {
            swapchain_ = VK_NULL_HANDLE;
            swapchain_format_ = VK_FORMAT_UNDEFINED;
            return;
        }
        if (wait_idle && context->device != VK_NULL_HANDLE) {
            (void)vkDeviceWaitIdle(context->device);
        }
        for (FrameSlot& frame : frames_) {
            if (frame.fence != VK_NULL_HANDLE) {
                vkDestroyFence(context->device, frame.fence, nullptr);
            }
            if (frame.render_finished != VK_NULL_HANDLE) {
                vkDestroySemaphore(
                    context->device, frame.render_finished, nullptr);
            }
            if (frame.image_available != VK_NULL_HANDLE) {
                vkDestroySemaphore(
                    context->device, frame.image_available, nullptr);
            }
            if (frame.command_pool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(
                    context->device, frame.command_pool, nullptr);
            }
            frame = {};
        }
        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(context->device, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
        swapchain_format_ = VK_FORMAT_UNDEFINED;
        for (ImageSlot& image : images_) {
            image = {};
        }
    }

    void destroy_swapchain_resources_locked(bool wait_idle = true) noexcept {
        if (context_ == nullptr) {
            swapchain_ = VK_NULL_HANDLE;
            swapchain_format_ = VK_FORMAT_UNDEFINED;
            return;
        }
        std::lock_guard<std::mutex> device_lock(context_->device_mutex);
        destroy_swapchain_resources_no_lock(context_, wait_idle);
    }

    void destroy_swapchain_locked() noexcept {
        destroy_swapchain_resources_locked();
    }

    mutable std::mutex mutex_;
    NativeWindowSwapchainSnapshot snapshot_;
    VulkanWsiSharedContext* context_{nullptr};
    VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
    VkFormat swapchain_format_{VK_FORMAT_UNDEFINED};
    std::array<ImageSlot, 16U> images_{};
    std::array<FrameSlot, 16U> frames_{};
#if defined(VK_KHR_incremental_present)
    std::array<VkRectLayerKHR, 64U> present_rects_{};
#else
    std::array<NativeDamageRect, 64U> present_rects_{};
#endif
    std::uint64_t bytes_per_image_{0U};
    std::uint64_t next_image_generation_{1U};
    std::uint64_t next_acquire_serial_{1U};
    std::uint64_t next_present_serial_{1U};
    std::uint64_t next_fence_value_{1U};
};

} // namespace

std::unique_ptr<NativeWindowSwapchainApi>
make_vulkan_native_window_swapchain_api() noexcept {
    try {
        return std::make_unique<VulkanNativeWindowSwapchainApi>();
    } catch (...) {
        return nullptr;
    }
}

bool native_vulkan_wsi_build_has_window_system(
    NativeWindowSystem system) noexcept {
#if defined(ZEVRYON_VULKAN_WSI_HAS_WIN32)
    if (system == NativeWindowSystem::Win32) {
        return true;
    }
#endif
#if defined(ZEVRYON_VULKAN_WSI_HAS_XCB)
    if (system == NativeWindowSystem::Xcb) {
        return true;
    }
#endif
#if defined(ZEVRYON_VULKAN_WSI_HAS_WAYLAND)
    if (system == NativeWindowSystem::Wayland) {
        return true;
    }
#endif
    (void)system;
    return false;
}

} // namespace zevryon::text

#endif // ZEVRYON_HAS_VULKAN_WSI
