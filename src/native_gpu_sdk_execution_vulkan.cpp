#include "native_gpu_sdk_execution.hpp"

#if defined(ZEVRYON_HAS_VULKAN_SDK)

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace zevryon::text {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::uint64_t kFenceTimeoutNs = 5'000'000'000ULL;

template <typename T>
void hash_value(std::uint64_t* hash, const T& value) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        *hash ^= bytes[index];
        *hash *= kFnvPrime;
    }
}

void clear_error(NativeGpuSdkError* error) noexcept {
    if (error != nullptr) {
        error->kind = NativeGpuSdkErrorKind::None;
        error->native_code = 0;
        error->message.clear();
    }
}

bool fail(
    NativeGpuSdkError* error,
    NativeGpuSdkErrorKind kind,
    const char* message,
    VkResult code = VK_SUCCESS) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->native_code = static_cast<std::int64_t>(code);
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

VkFormat map_format(GpuSurfaceFormat format) noexcept {
    return format == GpuSurfaceFormat::Rgba8Unorm
        ? VK_FORMAT_R8G8B8A8_UNORM
        : VK_FORMAT_B8G8R8A8_UNORM;
}

class VulkanNativeGpuSdkApi final : public NativeGpuSdkApi {
public:
    VulkanNativeGpuSdkApi() noexcept {
        snapshot_.probe.api_kind = NativeGpuApiKind::Vulkan;
        snapshot_.probe.availability = NativeGpuSdkAvailability::CompileOnly;
        snapshot_.probe.api_major = 1U;
        snapshot_.probe.flags = kNativeGpuSdkOffscreenSurface;
        std::uint32_t version = VK_API_VERSION_1_0;
#if defined(VK_VERSION_1_1)
        if (vkEnumerateInstanceVersion != nullptr) {
            (void)vkEnumerateInstanceVersion(&version);
        }
#endif
        snapshot_.probe.api_major = static_cast<std::uint16_t>(VK_API_VERSION_MAJOR(version));
        snapshot_.probe.api_minor = static_cast<std::uint16_t>(VK_API_VERSION_MINOR(version));
        snapshot_.probe.api_patch = static_cast<std::uint16_t>(VK_API_VERSION_PATCH(version));
    }

    ~VulkanNativeGpuSdkApi() override { shutdown(); }

    NativeGpuApiKind kind() const noexcept override { return NativeGpuApiKind::Vulkan; }

    NativeGpuSdkProbe probe() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_.probe;
    }

    bool initialize(
        const NativeGpuSdkConfig& config,
        NativeGpuSdkError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (config.api_kind != NativeGpuApiKind::Vulkan ||
            config.device_generation == 0U || config.runtime_generation == 0U ||
            config.window.system != NativeWindowSystem::Headless) {
            return fail(error, NativeGpuSdkErrorKind::InvalidInput,
                        "invalid Vulkan SDK configuration");
        }
        shutdown_locked();

        const VkApplicationInfo application_info{
            VK_STRUCTURE_TYPE_APPLICATION_INFO,
            nullptr,
            "Zevryon",
            1U,
            "Zevryon-Z2F8A",
            1U,
            VK_API_VERSION_1_0};
        const VkInstanceCreateInfo instance_info{
            VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            nullptr,
            0U,
            &application_info,
            0U,
            nullptr,
            0U,
            nullptr};
        VkResult result = vkCreateInstance(&instance_info, nullptr, &instance_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeGpuSdkErrorKind::RuntimeUnavailable,
                        "vkCreateInstance failed", result);
        }

        std::uint32_t physical_count = 0U;
        result = vkEnumeratePhysicalDevices(instance_, &physical_count, nullptr);
        if (result != VK_SUCCESS || physical_count == 0U) {
            shutdown_locked();
            return fail(error, NativeGpuSdkErrorKind::DeviceCreationFailed,
                        "no Vulkan physical device is available", result);
        }
        std::vector<VkPhysicalDevice> physical_devices(physical_count);
        result = vkEnumeratePhysicalDevices(
            instance_, &physical_count, physical_devices.data());
        if (result != VK_SUCCESS) {
            shutdown_locked();
            return fail(error, NativeGpuSdkErrorKind::DeviceCreationFailed,
                        "vkEnumeratePhysicalDevices failed", result);
        }

        bool found = false;
        VkPhysicalDeviceProperties selected_properties{};
        for (VkPhysicalDevice candidate : physical_devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU &&
                config.allow_software_device == 0U) {
                continue;
            }
            std::uint32_t queue_count = 0U;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, nullptr);
            std::vector<VkQueueFamilyProperties> queues(queue_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, queues.data());
            for (std::uint32_t index = 0U; index < queue_count; ++index) {
                if ((queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
                    physical_device_ = candidate;
                    queue_family_index_ = index;
                    selected_properties = properties;
                    found = true;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
        if (!found) {
            shutdown_locked();
            return fail(error, NativeGpuSdkErrorKind::QueueCreationFailed,
                        "no Vulkan graphics queue satisfies the policy");
        }

        const float priority = 1.0F;
        const VkDeviceQueueCreateInfo queue_info{
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            nullptr,
            0U,
            queue_family_index_,
            1U,
            &priority};
        const VkDeviceCreateInfo device_info{
            VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            nullptr,
            0U,
            1U,
            &queue_info,
            0U,
            nullptr,
            0U,
            nullptr,
            nullptr};
        result = vkCreateDevice(physical_device_, &device_info, nullptr, &device_);
        if (result != VK_SUCCESS) {
            shutdown_locked();
            return fail(error, NativeGpuSdkErrorKind::DeviceCreationFailed,
                        "vkCreateDevice failed", result);
        }
        vkGetDeviceQueue(device_, queue_family_index_, 0U, &queue_);

        const VkCommandPoolCreateInfo pool_info{
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            nullptr,
            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            queue_family_index_};
        result = vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_);
        if (result != VK_SUCCESS) {
            shutdown_locked();
            return fail(error, NativeGpuSdkErrorKind::QueueCreationFailed,
                        "vkCreateCommandPool failed", result);
        }
        const VkFenceCreateInfo fence_info{
            VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            nullptr,
            VK_FENCE_CREATE_SIGNALED_BIT};
        result = vkCreateFence(device_, &fence_info, nullptr, &submit_fence_);
        if (result != VK_SUCCESS) {
            shutdown_locked();
            return fail(error, NativeGpuSdkErrorKind::QueueCreationFailed,
                        "vkCreateFence failed", result);
        }

        config_ = config;
        snapshot_.config = config;
        snapshot_.probe.availability = NativeGpuSdkAvailability::RuntimeReady;
        snapshot_.probe.flags = kNativeGpuSdkRealDevice |
                                kNativeGpuSdkOffscreenSurface;
        if (selected_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
            snapshot_.probe.flags |= kNativeGpuSdkSoftwareDevice;
        }
        snapshot_.probe.vendor_id = selected_properties.vendorID;
        snapshot_.probe.device_id = selected_properties.deviceID;
        snapshot_.probe.queue_family_index = queue_family_index_;
        snapshot_.probe.runtime_generation = config.runtime_generation;
        std::uint64_t checksum = kFnvOffset;
        hash_value(&checksum, selected_properties.vendorID);
        hash_value(&checksum, selected_properties.deviceID);
        hash_value(&checksum, queue_family_index_);
        hash_value(&checksum, config.runtime_generation);
        snapshot_.probe.checksum = checksum;
        snapshot_.initialized_devices += 1U;
        initialized_ = true;
        return true;
    }

    bool configure_offscreen_surface(
        const GpuSurfaceDescriptor& surface,
        std::uint32_t image_count,
        NativeGpuSdkError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (!initialized_ || surface.surface_id == 0U ||
            surface.generation_id == 0U || surface.width == 0U ||
            surface.height == 0U || image_count == 0U ||
            image_count > config_.limits.maximum_swapchain_images ||
            image_count > images_.size()) {
            return fail(error, NativeGpuSdkErrorKind::InvalidInput,
                        "invalid Vulkan offscreen surface configuration");
        }
        destroy_images_locked();
        const VkFormat format = map_format(surface.format);
        std::uint64_t allocated_bytes = 0U;
        for (std::uint32_t index = 0U; index < image_count; ++index) {
            VkImageCreateInfo image_info{};
            image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.format = format;
            image_info.extent = VkExtent3D{surface.width, surface.height, 1U};
            image_info.mipLevels = 1U;
            image_info.arrayLayers = 1U;
            image_info.samples = VK_SAMPLE_COUNT_1_BIT;
            image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkResult result = vkCreateImage(device_, &image_info, nullptr, &images_[index].image);
            if (result != VK_SUCCESS) {
                destroy_images_locked();
                return fail(error, NativeGpuSdkErrorKind::ResourceAllocationFailed,
                            "vkCreateImage failed", result);
            }
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device_, images_[index].image, &requirements);
            const std::uint32_t memory_type = find_memory_type_locked(
                requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (memory_type == std::numeric_limits<std::uint32_t>::max()) {
                destroy_images_locked();
                return fail(error, NativeGpuSdkErrorKind::ResourceAllocationFailed,
                            "no Vulkan device-local memory type is available");
            }
            if (requirements.size > config_.limits.maximum_device_local_bytes - allocated_bytes) {
                destroy_images_locked();
                return fail(error, NativeGpuSdkErrorKind::ResourceBudgetExceeded,
                            "Vulkan offscreen image ring exceeds the device-local budget");
            }
            VkMemoryAllocateInfo allocation_info{};
            allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation_info.allocationSize = requirements.size;
            allocation_info.memoryTypeIndex = memory_type;
            result = vkAllocateMemory(device_, &allocation_info, nullptr, &images_[index].memory);
            if (result != VK_SUCCESS) {
                destroy_images_locked();
                return fail(error, NativeGpuSdkErrorKind::ResourceAllocationFailed,
                            "vkAllocateMemory failed", result);
            }
            result = vkBindImageMemory(device_, images_[index].image, images_[index].memory, 0U);
            if (result != VK_SUCCESS) {
                destroy_images_locked();
                return fail(error, NativeGpuSdkErrorKind::ResourceAllocationFailed,
                            "vkBindImageMemory failed", result);
            }
            images_[index].layout = VK_IMAGE_LAYOUT_UNDEFINED;
            images_[index].native_resource_id = next_resource_id_++;
            images_[index].generation = next_image_generation_++;
            images_[index].allocated_bytes = requirements.size;
            allocated_bytes += requirements.size;
        }
        surface_ = surface;
        image_count_ = image_count;
        next_image_index_ = 0U;
        snapshot_.surface = surface;
        snapshot_.configured_image_count = image_count;
        snapshot_.configured_surfaces += 1U;
        snapshot_.current_device_local_bytes = allocated_bytes;
        snapshot_.peak_device_local_bytes = std::max(
            snapshot_.peak_device_local_bytes, allocated_bytes);
        return true;
    }

    bool acquire_image(
        const GpuSurfaceDescriptor& surface,
        std::uint64_t ticket_id,
        NativePlatformSwapchainImage* image,
        NativeAcquireStatus* status,
        NativeGpuSdkError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (image == nullptr || status == nullptr || ticket_id == 0U ||
            image_count_ == 0U || !(surface == surface_)) {
            return fail(error, NativeGpuSdkErrorKind::AcquireFailed,
                        "invalid or stale Vulkan offscreen acquire request");
        }
        const std::uint32_t index = next_image_index_++ % image_count_;
        image->image.device_generation = config_.device_generation;
        image->image.surface_id = surface.surface_id;
        image->image.surface_generation = surface.generation_id;
        image->image.image_generation = images_[index].generation;
        image->image.image_index = index;
        image->image.flags = 0U;
        image->driver_generation = config_.runtime_generation;
        image->native_resource_id = images_[index].native_resource_id;
        image->state = NativePlatformResourceState::Present;
        image->reserved = 0U;
        *status = NativeAcquireStatus::Acquired;
        snapshot_.acquired_images += 1U;
        return true;
    }

    bool execute_submission(
        const NativePlatformSubmission& submission,
        NativeGpuSdkSubmissionReceipt* receipt,
        NativeGpuSdkError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (receipt == nullptr || !initialized_ || image_count_ == 0U ||
            submission.api_kind != NativeGpuApiKind::Vulkan ||
            !(submission.surface == surface_) ||
            submission.image.image.device_generation != config_.device_generation ||
            submission.image.driver_generation != config_.runtime_generation ||
            submission.image.image.image_index >= image_count_) {
            snapshot_.stale_rejections += 1U;
            return fail(error, NativeGpuSdkErrorKind::StaleGeneration,
                        "Vulkan submission references stale execution state");
        }
        if (submission.commands.size() > config_.limits.maximum_submission_commands ||
            submission.descriptors.size() > config_.limits.maximum_descriptors) {
            return fail(error, NativeGpuSdkErrorKind::ResourceBudgetExceeded,
                        "Vulkan submission exceeds bounded command or descriptor limits");
        }
        const std::uint32_t image_index = submission.image.image.image_index;
        ImageSlot& slot = images_[image_index];
        if (submission.image.native_resource_id != slot.native_resource_id ||
            submission.image.image.image_generation != slot.generation) {
            snapshot_.stale_rejections += 1U;
            return fail(error, NativeGpuSdkErrorKind::StaleGeneration,
                        "Vulkan acquired image generation is stale");
        }

        VkCommandBufferAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate_info.commandPool = command_pool_;
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate_info.commandBufferCount = 1U;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        VkResult result = vkAllocateCommandBuffers(device_, &allocate_info, &command_buffer);
        if (result != VK_SUCCESS) {
            return fail(error, NativeGpuSdkErrorKind::CommandEncodingFailed,
                        "vkAllocateCommandBuffers failed", result);
        }
        const auto free_command_buffer = [&]() noexcept {
            if (command_buffer != VK_NULL_HANDLE) {
                vkFreeCommandBuffers(device_, command_pool_, 1U, &command_buffer);
                command_buffer = VK_NULL_HANDLE;
            }
        };
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command_buffer, &begin_info);
        if (result != VK_SUCCESS) {
            free_command_buffer();
            return fail(error, NativeGpuSdkErrorKind::CommandEncodingFailed,
                        "vkBeginCommandBuffer failed", result);
        }

        transition_image_locked(command_buffer, slot, VK_IMAGE_LAYOUT_GENERAL);
        VkClearColorValue clear_color{};
        clear_color.float32[0] = static_cast<float>((submission.encoded_checksum >> 0U) & 0xFFU) / 255.0F;
        clear_color.float32[1] = static_cast<float>((submission.encoded_checksum >> 8U) & 0xFFU) / 255.0F;
        clear_color.float32[2] = static_cast<float>((submission.encoded_checksum >> 16U) & 0xFFU) / 255.0F;
        clear_color.float32[3] = 1.0F;
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0U;
        range.levelCount = 1U;
        range.baseArrayLayer = 0U;
        range.layerCount = 1U;
        vkCmdClearColorImage(
            command_buffer,
            slot.image,
            VK_IMAGE_LAYOUT_GENERAL,
            &clear_color,
            1U,
            &range);
        result = vkEndCommandBuffer(command_buffer);
        if (result != VK_SUCCESS) {
            free_command_buffer();
            return fail(error, NativeGpuSdkErrorKind::CommandEncodingFailed,
                        "vkEndCommandBuffer failed", result);
        }
        result = vkResetFences(device_, 1U, &submit_fence_);
        if (result != VK_SUCCESS) {
            free_command_buffer();
            return fail(error, NativeGpuSdkErrorKind::SubmissionFailed,
                        "vkResetFences failed", result);
        }
        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1U;
        submit_info.pCommandBuffers = &command_buffer;
        result = vkQueueSubmit(queue_, 1U, &submit_info, submit_fence_);
        if (result != VK_SUCCESS) {
            free_command_buffer();
            if (result == VK_ERROR_DEVICE_LOST) {
                snapshot_.device_lost_events += 1U;
                return fail(error, NativeGpuSdkErrorKind::DeviceLost,
                            "Vulkan device was lost during queue submission", result);
            }
            return fail(error, NativeGpuSdkErrorKind::SubmissionFailed,
                        "vkQueueSubmit failed", result);
        }
        result = vkWaitForFences(device_, 1U, &submit_fence_, VK_TRUE, kFenceTimeoutNs);
        free_command_buffer();
        if (result != VK_SUCCESS) {
            return fail(error, NativeGpuSdkErrorKind::SubmissionFailed,
                        "vkWaitForFences failed or timed out", result);
        }

        const std::uint64_t signal = next_fence_value_++;
        if (signal <= snapshot_.last_submitted_fence_value) {
            return fail(error, NativeGpuSdkErrorKind::FenceRegression,
                        "Vulkan software fence timeline regressed");
        }
        std::uint64_t checksum = kFnvOffset;
        hash_value(&checksum, submission.encoded_checksum);
        hash_value(&checksum, submission.frame_id);
        hash_value(&checksum, submission.ticket_id);
        hash_value(&checksum, submission.commands.size());
        hash_value(&checksum, submission.barriers.size());
        hash_value(&checksum, submission.descriptors.size());
        hash_value(&checksum, slot.native_resource_id);
        hash_value(&checksum, snapshot_.probe.vendor_id);
        hash_value(&checksum, snapshot_.probe.device_id);

        receipt->api_kind = NativeGpuApiKind::Vulkan;
        receipt->status = NativePresentStatus::Presented;
        receipt->command_count = static_cast<std::uint32_t>(submission.commands.size());
        receipt->barrier_count = static_cast<std::uint32_t>(submission.barriers.size());
        receipt->descriptor_count = static_cast<std::uint32_t>(submission.descriptors.size());
        receipt->image_index = image_index;
        receipt->device_generation = config_.device_generation;
        receipt->runtime_generation = config_.runtime_generation;
        receipt->surface_generation = surface_.generation_id;
        receipt->frame_id = submission.frame_id;
        receipt->ticket_id = submission.ticket_id;
        receipt->wait_fence_value = submission.wait_fence_value;
        receipt->signal_fence_value = signal;
        receipt->encoded_checksum = checksum;
        snapshot_.submitted_frames += 1U;
        snapshot_.in_flight_frame_count += 1U;
        snapshot_.last_submitted_fence_value = signal;
        return true;
    }

    bool retire_completed(
        std::uint64_t completed_fence_value,
        NativeGpuSdkError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (completed_fence_value < snapshot_.completed_fence_value ||
            completed_fence_value > snapshot_.last_submitted_fence_value) {
            return fail(error, NativeGpuSdkErrorKind::FenceRegression,
                        "Vulkan completion fence is outside the submitted timeline");
        }
        if (completed_fence_value > snapshot_.completed_fence_value) {
            const std::uint64_t delta = completed_fence_value - snapshot_.completed_fence_value;
            const std::uint64_t retired = std::min<std::uint64_t>(
                delta, snapshot_.in_flight_frame_count);
            snapshot_.retired_frames += retired;
            snapshot_.in_flight_frame_count -= static_cast<std::uint32_t>(retired);
            snapshot_.completed_fence_value = completed_fence_value;
        }
        return true;
    }

    NativeGpuSdkSnapshot snapshot() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

    void shutdown() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_locked();
    }

private:
    struct ImageSlot final {
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
        std::uint64_t native_resource_id{0};
        std::uint64_t generation{0};
        std::uint64_t allocated_bytes{0};
    };

    std::uint32_t find_memory_type_locked(
        std::uint32_t type_bits,
        VkMemoryPropertyFlags required) const noexcept {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physical_device_, &properties);
        for (std::uint32_t index = 0U; index < properties.memoryTypeCount; ++index) {
            if ((type_bits & (1U << index)) != 0U &&
                (properties.memoryTypes[index].propertyFlags & required) == required) {
                return index;
            }
        }
        return std::numeric_limits<std::uint32_t>::max();
    }

    void transition_image_locked(
        VkCommandBuffer command_buffer,
        ImageSlot& slot,
        VkImageLayout target) noexcept {
        if (slot.layout == target) {
            return;
        }
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = slot.layout;
        barrier.newLayout = target;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = slot.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0U;
        barrier.subresourceRange.levelCount = 1U;
        barrier.subresourceRange.baseArrayLayer = 0U;
        barrier.subresourceRange.layerCount = 1U;
        barrier.srcAccessMask = 0U;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT |
                                VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(
            command_buffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0U,
            0U,
            nullptr,
            0U,
            nullptr,
            1U,
            &barrier);
        slot.layout = target;
    }

    void destroy_images_locked() noexcept {
        if (device_ != VK_NULL_HANDLE) {
            (void)vkDeviceWaitIdle(device_);
            for (ImageSlot& slot : images_) {
                if (slot.image != VK_NULL_HANDLE) {
                    vkDestroyImage(device_, slot.image, nullptr);
                }
                if (slot.memory != VK_NULL_HANDLE) {
                    vkFreeMemory(device_, slot.memory, nullptr);
                }
                slot = {};
            }
        }
        image_count_ = 0U;
        snapshot_.configured_image_count = 0U;
        snapshot_.current_device_local_bytes = 0U;
        surface_ = {};
        snapshot_.surface = {};
    }

    void shutdown_locked() noexcept {
        destroy_images_locked();
        if (device_ != VK_NULL_HANDLE) {
            if (submit_fence_ != VK_NULL_HANDLE) {
                vkDestroyFence(device_, submit_fence_, nullptr);
            }
            if (command_pool_ != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device_, command_pool_, nullptr);
            }
            vkDestroyDevice(device_, nullptr);
        }
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
        }
        instance_ = VK_NULL_HANDLE;
        physical_device_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
        queue_ = VK_NULL_HANDLE;
        command_pool_ = VK_NULL_HANDLE;
        submit_fence_ = VK_NULL_HANDLE;
        queue_family_index_ = 0U;
        next_image_index_ = 0U;
        snapshot_.in_flight_frame_count = 0U;
        snapshot_.current_staging_bytes = 0U;
        snapshot_.last_submitted_fence_value = 0U;
        snapshot_.completed_fence_value = 0U;
        initialized_ = false;
    }

    mutable std::mutex mutex_;
    NativeGpuSdkSnapshot snapshot_;
    NativeGpuSdkConfig config_;
    GpuSurfaceDescriptor surface_;
    VkInstance instance_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue queue_{VK_NULL_HANDLE};
    VkCommandPool command_pool_{VK_NULL_HANDLE};
    VkFence submit_fence_{VK_NULL_HANDLE};
    std::array<ImageSlot, 16U> images_{};
    std::uint32_t queue_family_index_{0};
    std::uint32_t image_count_{0};
    std::uint32_t next_image_index_{0};
    std::uint64_t next_image_generation_{1};
    std::uint64_t next_resource_id_{1};
    std::uint64_t next_fence_value_{1};
    bool initialized_{false};
};

} // namespace

std::unique_ptr<NativeGpuSdkApi> make_vulkan_native_gpu_sdk_api() noexcept {
    try {
        return std::make_unique<VulkanNativeGpuSdkApi>();
    } catch (...) {
        return nullptr;
    }
}

bool native_gpu_sdk_build_has_vulkan_backend() noexcept {
    return true;
}

} // namespace zevryon::text

#endif // ZEVRYON_HAS_VULKAN_SDK
