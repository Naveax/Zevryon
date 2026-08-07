#include "native_shader_execution.hpp"

#if defined(ZEVRYON_HAS_VULKAN_SHADER_EXECUTION)

#include "native_shader_execution_vulkan_spirv.hpp"
#include "native_vulkan_wsi_context.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace zevryon::text {
namespace {

using detail::VulkanWsiSharedContext;

constexpr std::uint64_t kFenceTimeoutNanoseconds = 10'000'000'000ULL;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct VulkanBuffer final {
    VkBuffer handle{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkDeviceSize size{0U};
    void* mapped{nullptr};
    std::uint8_t coherent{0U};
};

struct VulkanImage final {
    VkImage handle{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::uint32_t layers{0U};
};

struct AtlasPageView final {
    const ShaderAtlasResidentPage* page{nullptr};
};

struct VulkanPushConstants final {
    std::array<std::uint32_t, 4U> a{};
    std::array<std::uint32_t, 4U> b{};
    std::array<std::uint32_t, 4U> c{};
    std::array<std::uint32_t, 4U> d{};
    std::array<std::uint32_t, 4U> e{};
    std::array<std::uint32_t, 4U> f{};
};
static_assert(sizeof(VulkanPushConstants) == 96U);

void clear_error(NativeShaderExecutionError* error) noexcept {
    if (error != nullptr) {
        error->kind = NativeShaderExecutionErrorKind::None;
        error->native_code = 0;
        error->message.clear();
    }
}

bool fail(
    NativeShaderExecutionError* error,
    NativeShaderExecutionErrorKind kind,
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

bool checked_multiply(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t* output) noexcept {
    if (output == nullptr ||
        (left != 0U && right >
            (std::numeric_limits<std::uint64_t>::max)() / left)) {
        return false;
    }
    *output = left * right;
    return true;
}

void hash_bytes(std::uint64_t* hash, std::span<const std::byte> bytes) noexcept {
    for (const std::byte value : bytes) {
        *hash ^= static_cast<std::uint8_t>(value);
        *hash *= kFnvPrime;
    }
}

template <typename T>
void hash_value(std::uint64_t* hash, const T& value) noexcept {
    hash_bytes(hash, std::as_bytes(std::span<const T>(&value, 1U)));
}

std::uint32_t pack_color(ShaderColorBgra8 color) noexcept {
    return static_cast<std::uint32_t>(color.blue) |
        (static_cast<std::uint32_t>(color.green) << 8U) |
        (static_cast<std::uint32_t>(color.red) << 16U) |
        (static_cast<std::uint32_t>(color.alpha) << 24U);
}

std::uint64_t image_resource_id(VkImage image) noexcept {
    static_assert(sizeof(VkImage) <= sizeof(std::uint64_t));
    std::uint64_t output = 0U;
    std::memcpy(&output, &image, sizeof(VkImage));
    return output;
}

bool find_memory_type(
    VkPhysicalDevice physical_device,
    std::uint32_t type_bits,
    VkMemoryPropertyFlags required,
    VkMemoryPropertyFlags preferred,
    std::uint32_t* index,
    std::uint8_t* coherent) noexcept {
    if (index == nullptr || coherent == nullptr) {
        return false;
    }
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
    std::uint32_t fallback = (std::numeric_limits<std::uint32_t>::max)();
    for (std::uint32_t candidate = 0U;
         candidate < properties.memoryTypeCount; ++candidate) {
        if ((type_bits & (1U << candidate)) == 0U) {
            continue;
        }
        const VkMemoryPropertyFlags flags =
            properties.memoryTypes[candidate].propertyFlags;
        if ((flags & required) != required) {
            continue;
        }
        if ((flags & preferred) == preferred) {
            *index = candidate;
            *coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U
                ? 1U : 0U;
            return true;
        }
        if (fallback == (std::numeric_limits<std::uint32_t>::max)()) {
            fallback = candidate;
        }
    }
    if (fallback == (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    *index = fallback;
    *coherent =
        (properties.memoryTypes[fallback].propertyFlags &
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U ? 1U : 0U;
    return true;
}

void destroy_buffer(VkDevice device, VulkanBuffer* buffer) noexcept {
    if (buffer == nullptr || device == VK_NULL_HANDLE) {
        return;
    }
    if (buffer->mapped != nullptr && buffer->memory != VK_NULL_HANDLE) {
        vkUnmapMemory(device, buffer->memory);
    }
    if (buffer->handle != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, buffer->handle, nullptr);
    }
    if (buffer->memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, buffer->memory, nullptr);
    }
    *buffer = {};
}

void destroy_image(VkDevice device, VulkanImage* image) noexcept {
    if (image == nullptr || device == VK_NULL_HANDLE) {
        return;
    }
    if (image->view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, image->view, nullptr);
    }
    if (image->handle != VK_NULL_HANDLE) {
        vkDestroyImage(device, image->handle, nullptr);
    }
    if (image->memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, image->memory, nullptr);
    }
    *image = {};
}

bool create_buffer(
    VulkanWsiSharedContext* context,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags required,
    VkMemoryPropertyFlags preferred,
    bool map,
    VulkanBuffer* output,
    NativeShaderExecutionError* error) noexcept {
    if (context == nullptr || output == nullptr || size == 0U) {
        return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                    "invalid Vulkan buffer request");
    }
    VulkanBuffer staged{};
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = vkCreateBuffer(
        context->device, &buffer_info, nullptr, &staged.handle);
    if (result != VK_SUCCESS) {
        return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                    "vkCreateBuffer failed", result);
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context->device, staged.handle, &requirements);
    std::uint32_t memory_type = 0U;
    if (!find_memory_type(
            context->physical_device, requirements.memoryTypeBits,
            required, preferred, &memory_type, &staged.coherent)) {
        destroy_buffer(context->device, &staged);
        return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                    "no compatible Vulkan buffer memory type");
    }
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    result = vkAllocateMemory(
        context->device, &allocation, nullptr, &staged.memory);
    if (result != VK_SUCCESS) {
        destroy_buffer(context->device, &staged);
        return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                    "vkAllocateMemory for buffer failed", result);
    }
    result = vkBindBufferMemory(
        context->device, staged.handle, staged.memory, 0U);
    if (result != VK_SUCCESS) {
        destroy_buffer(context->device, &staged);
        return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                    "vkBindBufferMemory failed", result);
    }
    if (map) {
        result = vkMapMemory(
            context->device, staged.memory, 0U, size, 0U, &staged.mapped);
        if (result != VK_SUCCESS) {
            destroy_buffer(context->device, &staged);
            return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                        "vkMapMemory failed", result);
        }
    }
    staged.size = size;
    *output = staged;
    return true;
}

bool create_image(
    VulkanWsiSharedContext* context,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t layers,
    VkImageUsageFlags usage,
    VkImageViewType view_type,
    VulkanImage* output,
    NativeShaderExecutionError* error) noexcept {
    if (context == nullptr || output == nullptr ||
        width == 0U || height == 0U || layers == 0U) {
        return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                    "invalid Vulkan image request");
    }
    VulkanImage staged{};
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R32_UINT;
    image_info.extent = {width, height, 1U};
    image_info.mipLevels = 1U;
    image_info.arrayLayers = layers;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult result = vkCreateImage(
        context->device, &image_info, nullptr, &staged.handle);
    if (result != VK_SUCCESS) {
        return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                    "vkCreateImage failed", result);
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(context->device, staged.handle, &requirements);
    std::uint32_t memory_type = 0U;
    std::uint8_t coherent = 0U;
    if (!find_memory_type(
            context->physical_device, requirements.memoryTypeBits, 0U,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            &memory_type, &coherent)) {
        destroy_image(context->device, &staged);
        return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                    "no compatible Vulkan image memory type");
    }
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    result = vkAllocateMemory(
        context->device, &allocation, nullptr, &staged.memory);
    if (result != VK_SUCCESS) {
        destroy_image(context->device, &staged);
        return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                    "vkAllocateMemory for image failed", result);
    }
    result = vkBindImageMemory(
        context->device, staged.handle, staged.memory, 0U);
    if (result != VK_SUCCESS) {
        destroy_image(context->device, &staged);
        return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                    "vkBindImageMemory failed", result);
    }
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = staged.handle;
    view_info.viewType = view_type;
    view_info.format = VK_FORMAT_R32_UINT;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0U;
    view_info.subresourceRange.levelCount = 1U;
    view_info.subresourceRange.baseArrayLayer = 0U;
    view_info.subresourceRange.layerCount = layers;
    result = vkCreateImageView(
        context->device, &view_info, nullptr, &staged.view);
    if (result != VK_SUCCESS) {
        destroy_image(context->device, &staged);
        return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                    "vkCreateImageView failed", result);
    }
    staged.width = width;
    staged.height = height;
    staged.layers = layers;
    *output = staged;
    return true;
}

VkImageMemoryBarrier image_barrier(
    VkImage image,
    VkImageLayout old_layout,
    VkImageLayout new_layout,
    VkAccessFlags source_access,
    VkAccessFlags destination_access,
    std::uint32_t layers) noexcept {
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
    barrier.subresourceRange.layerCount = layers;
    return barrier;
}

class VulkanShaderExecutor final : public NativeShaderExecutor {
public:
    ~VulkanShaderExecutor() override { shutdown(); }

    NativeGpuApiKind kind() const noexcept override {
        return NativeGpuApiKind::Vulkan;
    }

    bool configure(
        const NativeShaderExecutionConfig& config,
        NativeShaderExecutionError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (config.context.api_kind != NativeGpuApiKind::Vulkan ||
            config.context.device_generation == 0U ||
            config.context.runtime_generation == 0U ||
            config.executor_generation == 0U ||
            config.limits.maximum_commands == 0U ||
            config.limits.maximum_fill_instances == 0U ||
            config.limits.maximum_glyph_instances == 0U ||
            config.limits.maximum_atlas_pages == 0U ||
            config.limits.maximum_surface_width == 0U ||
            config.limits.maximum_surface_height == 0U ||
            config.limits.maximum_packet_bytes == 0U ||
            config.limits.maximum_atlas_bytes == 0U ||
            config.limits.maximum_readback_bytes == 0U ||
            (config.context.flags & kNativeGpuSdkContextDeviceValid) == 0U ||
            (config.context.flags & kNativeGpuSdkContextGraphicsQueueValid) == 0U) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "invalid Vulkan shader executor configuration");
        }
        reset_locked();
        VulkanWsiSharedContext* retained =
            detail::retain_vulkan_wsi_context(config.context);
        if (retained == nullptr) {
            return fail(error, NativeShaderExecutionErrorKind::NativeContextUnavailable,
                        "retained Vulkan WSI context is unavailable");
        }
        context_ = retained;
        bool configured = false;
        {
            std::lock_guard<std::mutex> device_lock(context_->device_mutex);
            configured = create_pipeline_locked(error);
        }
        if (!configured) {
            VulkanWsiSharedContext* released = context_;
            {
                std::lock_guard<std::mutex> device_lock(released->device_mutex);
                destroy_resources_locked();
            }
            context_ = nullptr;
            detail::release_vulkan_wsi_context(released);
            return false;
        }
        config_ = config;
        snapshot_ = {};
        snapshot_.api_kind = NativeGpuApiKind::Vulkan;
        snapshot_.configured = 1U;
        snapshot_.capability_flags =
            kNativeShaderExecutionIntegerComposition |
            kNativeShaderExecutionPersistentAtlas |
            kNativeShaderExecutionGpuReadback |
            kNativeShaderExecutionRetainedContext |
            kNativeShaderExecutionDirectSurfaceExport;
        snapshot_.device_generation = config.context.device_generation;
        snapshot_.runtime_generation = config.context.runtime_generation;
        snapshot_.executor_generation = config.executor_generation;
        return true;
    }

    bool execute(
        const GpuShaderPacket& packet,
        const ShaderAtlasResidency& atlas,
        ShaderReadback* readback,
        NativeShaderExecutionError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (context_ == nullptr || snapshot_.configured == 0U ||
            packet.header.frame_id == 0U ||
            packet.header.packet_checksum != shader_packet_checksum(packet) ||
            packet.header.command_count != packet.commands.size() ||
            packet.header.fill_instance_count != packet.fills.size() ||
            packet.header.glyph_instance_count != packet.glyphs.size() ||
            packet.header.scissor_count != packet.scissors.size() ||
            packet.header.surface_width == 0U ||
            packet.header.surface_height == 0U) {
            snapshot_.rejected_packets += 1U;
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "invalid or mutated Vulkan shader packet");
        }
        if (packet.commands.size() > config_.limits.maximum_commands ||
            packet.fills.size() > config_.limits.maximum_fill_instances ||
            packet.glyphs.size() > config_.limits.maximum_glyph_instances ||
            packet.header.packet_bytes > config_.limits.maximum_packet_bytes ||
            packet.header.surface_width > config_.limits.maximum_surface_width ||
            packet.header.surface_height > config_.limits.maximum_surface_height) {
            snapshot_.rejected_packets += 1U;
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "shader packet exceeds Vulkan execution limits");
        }
        try {
            std::lock_guard<std::mutex> device_lock(context_->device_mutex);
            if (context_->device == VK_NULL_HANDLE ||
                context_->graphics_queue == VK_NULL_HANDLE ||
                context_->physical_device == VK_NULL_HANDLE ||
                context_->device_generation != config_.context.device_generation ||
                context_->runtime_generation != config_.context.runtime_generation) {
                snapshot_.rejected_packets += 1U;
                return fail(error, NativeShaderExecutionErrorKind::StaleGeneration,
                            "Vulkan shader executor context generation is stale");
            }
            std::vector<AtlasPageView> pages;
            if (!collect_pages_locked(packet, atlas, &pages, error) ||
                !ensure_atlas_locked(pages, error) ||
                !ensure_output_locked(
                    packet.header.surface_width,
                    packet.header.surface_height,
                    readback != nullptr,
                    error) ||
                !update_descriptors_locked(error)) {
                return false;
            }
            if (!record_execute_locked(packet, readback != nullptr, error)) {
                output_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
                return false;
            }
            if (!submit_and_wait_locked(error)) {
                destroy_image(context_->device, &output_);
                destroy_buffer(context_->device, &readback_);
                snapshot_.output_surface_bytes = 0U;
                last_surface_ = {};
                return false;
            }
            if (readback != nullptr &&
                !publish_readback_locked(packet, readback, error)) {
                return false;
            }

            last_surface_ = {};
            last_surface_.api_kind = NativeGpuApiKind::Vulkan;
            last_surface_.format = GpuSurfaceFormat::Bgra8Unorm;
            last_surface_.state = NativeShaderSurfaceState::ShaderRead;
            last_surface_.flags =
                kNativeShaderSurfaceReady |
                kNativeShaderSurfaceNonOwning |
                kNativeShaderSurfacePremultipliedAlpha;
            last_surface_.device_generation = config_.context.device_generation;
            last_surface_.runtime_generation = config_.context.runtime_generation;
            last_surface_.executor_generation = config_.executor_generation;
            last_surface_.output_generation = output_generation_;
            last_surface_.frame_id = packet.header.frame_id;
            last_surface_.content_checksum = packet.header.packet_checksum;
            last_surface_.native_resource = image_resource_id(output_.handle);
            last_surface_.width = packet.header.surface_width;
            last_surface_.height = packet.header.surface_height;

            snapshot_.executions += 1U;
            snapshot_.last_packet_checksum = packet.header.packet_checksum;
            if (readback != nullptr) {
                snapshot_.readbacks += 1U;
                snapshot_.last_readback_checksum = readback->checksum;
            }
            return true;
        } catch (const std::bad_alloc&) {
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "Vulkan shader execution allocation failed");
        } catch (...) {
            return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                        "unexpected Vulkan shader execution failure");
        }
    }

    bool export_surface(
        NativeShaderSurfaceView* surface,
        NativeShaderExecutionError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (surface == nullptr) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "shader surface output is null");
        }
        if (!native_shader_surface_view_valid(last_surface_) ||
            output_.handle == VK_NULL_HANDLE ||
            output_.layout != VK_IMAGE_LAYOUT_GENERAL ||
            image_resource_id(output_.handle) != last_surface_.native_resource) {
            *surface = {};
            return fail(error, NativeShaderExecutionErrorKind::StaleGeneration,
                        "no completed Vulkan shader surface is available");
        }
        *surface = last_surface_;
        return true;
    }

    NativeShaderExecutionSnapshot snapshot() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

    void shutdown() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        reset_locked();
    }

private:
    bool create_pipeline_locked(NativeShaderExecutionError* error) noexcept {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = context_->graphics_queue_family;
        VkResult result = vkCreateCommandPool(
            context_->device, &pool_info, nullptr, &command_pool_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "vkCreateCommandPool failed", result);
        }
        VkCommandBufferAllocateInfo command_info{};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_info.commandPool = command_pool_;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1U;
        result = vkAllocateCommandBuffers(
            context_->device, &command_info, &command_buffer_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "vkAllocateCommandBuffers failed", result);
        }
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        result = vkCreateFence(
            context_->device, &fence_info, nullptr, &fence_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "vkCreateFence failed", result);
        }

        const std::array<VkDescriptorSetLayoutBinding, 2U> bindings{{
            {0U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1U,
             VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1U,
             VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layout_info.pBindings = bindings.data();
        result = vkCreateDescriptorSetLayout(
            context_->device, &layout_info, nullptr, &descriptor_set_layout_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "vkCreateDescriptorSetLayout failed", result);
        }
        const VkDescriptorPoolSize pool_size{
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2U};
        VkDescriptorPoolCreateInfo pool_create{};
        pool_create.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_create.maxSets = 1U;
        pool_create.poolSizeCount = 1U;
        pool_create.pPoolSizes = &pool_size;
        result = vkCreateDescriptorPool(
            context_->device, &pool_create, nullptr, &descriptor_pool_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "vkCreateDescriptorPool failed", result);
        }
        VkDescriptorSetAllocateInfo set_info{};
        set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        set_info.descriptorPool = descriptor_pool_;
        set_info.descriptorSetCount = 1U;
        set_info.pSetLayouts = &descriptor_set_layout_;
        result = vkAllocateDescriptorSets(
            context_->device, &set_info, &descriptor_set_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "vkAllocateDescriptorSets failed", result);
        }
        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push_range.offset = 0U;
        push_range.size = sizeof(VulkanPushConstants);
        VkPipelineLayoutCreateInfo pipeline_layout_info{};
        pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_info.setLayoutCount = 1U;
        pipeline_layout_info.pSetLayouts = &descriptor_set_layout_;
        pipeline_layout_info.pushConstantRangeCount = 1U;
        pipeline_layout_info.pPushConstantRanges = &push_range;
        result = vkCreatePipelineLayout(
            context_->device, &pipeline_layout_info, nullptr, &pipeline_layout_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "vkCreatePipelineLayout failed", result);
        }
        VkShaderModuleCreateInfo shader_info{};
        shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader_info.codeSize = detail::kVulkanIntegerComposerSpirv.size() *
            sizeof(std::uint32_t);
        shader_info.pCode = detail::kVulkanIntegerComposerSpirv.data();
        VkShaderModule shader = VK_NULL_HANDLE;
        result = vkCreateShaderModule(
            context_->device, &shader_info, nullptr, &shader);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::ShaderCompilationFailed,
                        "vkCreateShaderModule failed", result);
        }
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.stage = stage;
        pipeline_info.layout = pipeline_layout_;
        result = vkCreateComputePipelines(
            context_->device, VK_NULL_HANDLE, 1U,
            &pipeline_info, nullptr, &pipeline_);
        vkDestroyShaderModule(context_->device, shader, nullptr);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "vkCreateComputePipelines failed", result);
        }
        return true;
    }

    bool collect_pages_locked(
        const GpuShaderPacket& packet,
        const ShaderAtlasResidency& atlas,
        std::vector<AtlasPageView>* output,
        NativeShaderExecutionError* error) noexcept {
        if (output == nullptr) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "Vulkan atlas output is null");
        }
        output->clear();
        for (const GpuShaderGlyphInstance& glyph : packet.glyphs) {
            if (glyph.atlas_page_index >= config_.limits.maximum_atlas_pages) {
                return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                            "Vulkan glyph atlas page exceeds execution limits");
            }
            const ShaderAtlasResidentPage* page = atlas.find(
                glyph.atlas_page_index, glyph.atlas_page_generation);
            if (page == nullptr || page->width == 0U || page->height == 0U) {
                return fail(error, NativeShaderExecutionErrorKind::StaleGeneration,
                            "Vulkan glyph references a missing atlas generation");
            }
            const auto match = std::find_if(
                output->begin(), output->end(),
                [page](const AtlasPageView& current) {
                    return current.page->page_index == page->page_index;
                });
            if (match != output->end()) {
                if (match->page->page_generation != page->page_generation) {
                    return fail(error, NativeShaderExecutionErrorKind::StaleGeneration,
                                "mixed Vulkan atlas generations for one page");
                }
                continue;
            }
            output->push_back({page});
        }
        std::sort(
            output->begin(), output->end(),
            [](const AtlasPageView& left, const AtlasPageView& right) {
                return left.page->page_index < right.page->page_index;
            });
        return true;
    }

    bool ensure_atlas_locked(
        const std::vector<AtlasPageView>& pages,
        NativeShaderExecutionError* error) noexcept {
        std::uint64_t signature = kFnvOffset;
        std::uint64_t resident_bytes = 0U;
        std::uint32_t width = 1U;
        std::uint32_t height = 1U;
        std::uint32_t layers = 1U;
        for (const AtlasPageView& view : pages) {
            const ShaderAtlasResidentPage& page = *view.page;
            std::uint64_t expected = 0U;
            if (!checked_multiply(page.width, page.height, &expected) ||
                !checked_multiply(expected, 4U, &expected) ||
                expected != page.canonical_bgra.size()) {
                return fail(error, NativeShaderExecutionErrorKind::AtlasUploadFailed,
                            "Vulkan canonical atlas page size is invalid");
            }
            if (resident_bytes > config_.limits.maximum_atlas_bytes - expected) {
                return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                            "Vulkan resident atlas exceeds execution limits");
            }
            resident_bytes += expected;
            width = std::max(width, static_cast<std::uint32_t>(page.width));
            height = std::max(height, static_cast<std::uint32_t>(page.height));
            layers = std::max(layers, page.page_index + 1U);
            hash_value(&signature, page.page_index);
            hash_value(&signature, page.page_generation);
            hash_value(&signature, page.width);
            hash_value(&signature, page.height);
            const std::uint64_t page_checksum = shader_bytes_checksum(
                std::span<const std::byte>(page.canonical_bgra));
            hash_value(&signature, page_checksum);
        }
        if (atlas_.handle != VK_NULL_HANDLE && atlas_signature_ == signature &&
            atlas_.width == width && atlas_.height == height &&
            atlas_.layers == layers) {
            snapshot_.atlas_reuses += 1U;
            return true;
        }
        if (vkDeviceWaitIdle(context_->device) != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::DeviceLost,
                        "vkDeviceWaitIdle before atlas rebuild failed");
        }
        destroy_image(context_->device, &atlas_);
        if (!create_image(
                context_, width, height, layers,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                &atlas_, error)) {
            return false;
        }
        std::uint64_t staging_size = resident_bytes == 0U ? 4U : resident_bytes;
        VulkanBuffer staging{};
        if (!create_buffer(
                context_, staging_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                true, &staging, error)) {
            destroy_image(context_->device, &atlas_);
            return false;
        }
        std::memset(staging.mapped, 0, static_cast<std::size_t>(staging_size));
        std::vector<VkBufferImageCopy> copies;
        copies.reserve(pages.size());
        VkDeviceSize cursor = 0U;
        for (const AtlasPageView& view : pages) {
            const ShaderAtlasResidentPage& page = *view.page;
            std::memcpy(
                static_cast<std::byte*>(staging.mapped) +
                    static_cast<std::size_t>(cursor),
                page.canonical_bgra.data(), page.canonical_bgra.size());
            VkBufferImageCopy copy{};
            copy.bufferOffset = cursor;
            copy.bufferRowLength = 0U;
            copy.bufferImageHeight = 0U;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.mipLevel = 0U;
            copy.imageSubresource.baseArrayLayer = page.page_index;
            copy.imageSubresource.layerCount = 1U;
            copy.imageExtent = {page.width, page.height, 1U};
            copies.push_back(copy);
            cursor += static_cast<VkDeviceSize>(page.canonical_bgra.size());
        }
        if (staging.coherent == 0U) {
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = staging.memory;
            range.offset = 0U;
            range.size = VK_WHOLE_SIZE;
            const VkResult flush_result = vkFlushMappedMemoryRanges(
                context_->device, 1U, &range);
            if (flush_result != VK_SUCCESS) {
                destroy_buffer(context_->device, &staging);
                destroy_image(context_->device, &atlas_);
                return fail(error, NativeShaderExecutionErrorKind::AtlasUploadFailed,
                            "vkFlushMappedMemoryRanges for atlas failed",
                            flush_result);
            }
        }
        if (!begin_commands_locked(error)) {
            destroy_buffer(context_->device, &staging);
            destroy_image(context_->device, &atlas_);
            return false;
        }
        const VkImageMemoryBarrier to_transfer = image_barrier(
            atlas_.handle, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0U, VK_ACCESS_TRANSFER_WRITE_BIT, atlas_.layers);
        vkCmdPipelineBarrier(
            command_buffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0U,
            0U, nullptr, 0U, nullptr, 1U, &to_transfer);
        if (!copies.empty()) {
            vkCmdCopyBufferToImage(
                command_buffer_, staging.handle, atlas_.handle,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                static_cast<std::uint32_t>(copies.size()), copies.data());
        }
        const VkImageMemoryBarrier to_general = image_barrier(
            atlas_.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            atlas_.layers);
        vkCmdPipelineBarrier(
            command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U,
            0U, nullptr, 0U, nullptr, 1U, &to_general);
        atlas_.layout = VK_IMAGE_LAYOUT_GENERAL;
        if (!submit_and_wait_locked(error)) {
            destroy_buffer(context_->device, &staging);
            destroy_image(context_->device, &atlas_);
            return false;
        }
        destroy_buffer(context_->device, &staging);
        atlas_signature_ = signature;
        snapshot_.atlas_upload_batches += 1U;
        snapshot_.persistent_atlas_bytes = resident_bytes;
        snapshot_.peak_transient_bytes = std::max(
            snapshot_.peak_transient_bytes, staging_size);
        return true;
    }

    bool ensure_output_locked(
        std::uint32_t width,
        std::uint32_t height,
        bool require_readback,
        NativeShaderExecutionError* error) noexcept {
        std::uint64_t bytes = 0U;
        if (!checked_multiply(width, height, &bytes) ||
            !checked_multiply(bytes, 4U, &bytes)) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "Vulkan output surface size overflowed");
        }

        const bool output_matches =
            output_.handle != VK_NULL_HANDLE &&
            output_.width == width &&
            output_.height == height;

        if (!output_matches) {
            if (vkDeviceWaitIdle(context_->device) != VK_SUCCESS) {
                return fail(error, NativeShaderExecutionErrorKind::DeviceLost,
                            "vkDeviceWaitIdle before output rebuild failed");
            }

            destroy_image(context_->device, &output_);
            last_surface_ = {};
            snapshot_.output_surface_bytes = 0U;

            if (!create_image(
                    context_, width, height, 1U,
                    VK_IMAGE_USAGE_STORAGE_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_VIEW_TYPE_2D,
                    &output_, error)) {
                return false;
            }

            if (next_output_generation_ ==
                (std::numeric_limits<std::uint64_t>::max)()) {
                destroy_image(context_->device, &output_);
                return fail(
                    error,
                    NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                    "Vulkan shader output generation overflowed");
            }

            output_generation_ = next_output_generation_++;
            snapshot_.output_surface_bytes = bytes;
        }

        if (!require_readback) {
            return true;
        }

        if (bytes > config_.limits.maximum_readback_bytes) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "Vulkan shader readback exceeds readback limits");
        }

        if (readback_.handle != VK_NULL_HANDLE &&
            readback_.mapped != nullptr &&
            readback_.size >= bytes) {
            return true;
        }

        if (vkDeviceWaitIdle(context_->device) != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::DeviceLost,
                        "vkDeviceWaitIdle before readback allocation failed");
        }

        destroy_buffer(context_->device, &readback_);
        if (!create_buffer(
                context_, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                true, &readback_, error)) {
            return false;
        }

        snapshot_.peak_transient_bytes = std::max(
            snapshot_.peak_transient_bytes, bytes);
        return true;
    }

    bool update_descriptors_locked(
        NativeShaderExecutionError* error) noexcept {
        if (atlas_.view == VK_NULL_HANDLE || output_.view == VK_NULL_HANDLE) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                        "Vulkan shader descriptor image view is unavailable");
        }
        const std::array<VkDescriptorImageInfo, 2U> images{{
            {VK_NULL_HANDLE, atlas_.view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, output_.view, VK_IMAGE_LAYOUT_GENERAL}}};
        std::array<VkWriteDescriptorSet, 2U> writes{};
        for (std::uint32_t index = 0U; index < writes.size(); ++index) {
            writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index].dstSet = descriptor_set_;
            writes[index].dstBinding = index;
            writes[index].descriptorCount = 1U;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[index].pImageInfo = &images[index];
        }
        vkUpdateDescriptorSets(
            context_->device, static_cast<std::uint32_t>(writes.size()),
            writes.data(), 0U, nullptr);
        return true;
    }

    bool begin_commands_locked(NativeShaderExecutionError* error) noexcept {
        VkResult result = vkResetFences(context_->device, 1U, &fence_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                        "vkResetFences failed", result);
        }
        result = vkResetCommandPool(
            context_->device, command_pool_, 0U);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                        "vkResetCommandPool failed", result);
        }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command_buffer_, &begin);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                        "vkBeginCommandBuffer failed", result);
        }
        return true;
    }

    bool submit_and_wait_locked(NativeShaderExecutionError* error) noexcept {
        VkResult result = vkEndCommandBuffer(command_buffer_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                        "vkEndCommandBuffer failed", result);
        }
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1U;
        submit.pCommandBuffers = &command_buffer_;
        result = vkQueueSubmit(
            context_->graphics_queue, 1U, &submit, fence_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::SubmissionFailed,
                        "vkQueueSubmit for shader execution failed", result);
        }
        result = vkWaitForFences(
            context_->device, 1U, &fence_, VK_TRUE,
            kFenceTimeoutNanoseconds);
        if (result == VK_TIMEOUT) {
            return fail(error, NativeShaderExecutionErrorKind::FenceTimeout,
                        "Vulkan shader execution fence timed out", result);
        }
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::DeviceLost,
                        "vkWaitForFences failed", result);
        }
        return true;
    }

    static bool clipped_dispatch(
        const ShaderRectI& destination,
        const ShaderRectI& scissor,
        std::uint32_t surface_width,
        std::uint32_t surface_height,
        std::uint32_t* x,
        std::uint32_t* y,
        std::uint32_t* width,
        std::uint32_t* height) noexcept {
        const std::int64_t x0 = std::max<std::int64_t>(
            0, std::max<std::int64_t>(destination.x, scissor.x));
        const std::int64_t y0 = std::max<std::int64_t>(
            0, std::max<std::int64_t>(destination.y, scissor.y));
        const std::int64_t destination_x1 =
            static_cast<std::int64_t>(destination.x) + destination.width;
        const std::int64_t destination_y1 =
            static_cast<std::int64_t>(destination.y) + destination.height;
        const std::int64_t scissor_x1 =
            static_cast<std::int64_t>(scissor.x) + scissor.width;
        const std::int64_t scissor_y1 =
            static_cast<std::int64_t>(scissor.y) + scissor.height;
        const std::int64_t x1 = std::min<std::int64_t>(
            surface_width, std::min(destination_x1, scissor_x1));
        const std::int64_t y1 = std::min<std::int64_t>(
            surface_height, std::min(destination_y1, scissor_y1));
        if (x1 <= x0 || y1 <= y0) {
            return false;
        }
        *x = static_cast<std::uint32_t>(x0);
        *y = static_cast<std::uint32_t>(y0);
        *width = static_cast<std::uint32_t>(x1 - x0);
        *height = static_cast<std::uint32_t>(y1 - y0);
        return true;
    }

    void dispatch_barrier_locked() noexcept {
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(
            command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U,
            1U, &barrier, 0U, nullptr, 0U, nullptr);
    }

    void dispatch_locked(const VulkanPushConstants& constants) noexcept {
        vkCmdPushConstants(
            command_buffer_, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
            0U, sizeof(constants), &constants);
        vkCmdDispatch(
            command_buffer_,
            (constants.b[2] + 7U) / 8U,
            (constants.b[3] + 7U) / 8U,
            1U);
        dispatch_barrier_locked();
    }

    bool record_execute_locked(
        const GpuShaderPacket& packet,
        bool capture_readback,
        NativeShaderExecutionError* error) noexcept {
        if (!begin_commands_locked(error)) {
            return false;
        }
        if (output_.layout != VK_IMAGE_LAYOUT_GENERAL) {
            const VkImageMemoryBarrier to_general = image_barrier(
                output_.handle, output_.layout, VK_IMAGE_LAYOUT_GENERAL,
                0U, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, 1U);
            vkCmdPipelineBarrier(
                command_buffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U,
                0U, nullptr, 0U, nullptr, 1U, &to_general);
            output_.layout = VK_IMAGE_LAYOUT_GENERAL;
        }
        vkCmdBindPipeline(
            command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(
            command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
            pipeline_layout_, 0U, 1U, &descriptor_set_, 0U, nullptr);
        VulkanPushConstants clear{};
        clear.a[0] = packet.header.surface_width;
        clear.a[1] = packet.header.surface_height;
        clear.a[2] = 0U;
        clear.b[2] = packet.header.surface_width;
        clear.b[3] = packet.header.surface_height;
        dispatch_locked(clear);

        for (const GpuShaderDrawCommand& command : packet.commands) {
            if (command.scissor_index >= packet.scissors.size()) {
                return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                            "Vulkan shader command scissor index is invalid");
            }
            const ShaderRectI scissor = packet.scissors[command.scissor_index].rect;
            if (command.kind == ShaderPrimitiveKind::Fill) {
                if (command.first_instance > packet.fills.size() ||
                    command.instance_count >
                        packet.fills.size() - command.first_instance) {
                    return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                                "Vulkan fill command range is invalid");
                }
                for (std::uint32_t offset = 0U;
                     offset < command.instance_count; ++offset) {
                    const GpuShaderFillInstance& fill =
                        packet.fills[command.first_instance + offset];
                    std::uint32_t x = 0U;
                    std::uint32_t y = 0U;
                    std::uint32_t width = 0U;
                    std::uint32_t height = 0U;
                    if (!clipped_dispatch(
                            fill.destination, scissor,
                            packet.header.surface_width,
                            packet.header.surface_height,
                            &x, &y, &width, &height)) {
                        continue;
                    }
                    VulkanPushConstants constants{};
                    constants.a = {
                        packet.header.surface_width,
                        packet.header.surface_height, 1U, 0U};
                    constants.b = {x, y, width, height};
                    constants.c = {
                        static_cast<std::uint32_t>(fill.destination.x),
                        static_cast<std::uint32_t>(fill.destination.y),
                        static_cast<std::uint32_t>(fill.destination.width),
                        static_cast<std::uint32_t>(fill.destination.height)};
                    constants.e[1] = pack_color(fill.color);
                    dispatch_locked(constants);
                }
            } else if (command.kind == ShaderPrimitiveKind::GlyphBatch) {
                if (command.first_instance > packet.glyphs.size() ||
                    command.instance_count >
                        packet.glyphs.size() - command.first_instance) {
                    return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                                "Vulkan glyph command range is invalid");
                }
                for (std::uint32_t offset = 0U;
                     offset < command.instance_count; ++offset) {
                    const GpuShaderGlyphInstance& glyph =
                        packet.glyphs[command.first_instance + offset];
                    std::uint32_t x = 0U;
                    std::uint32_t y = 0U;
                    std::uint32_t width = 0U;
                    std::uint32_t height = 0U;
                    if (!clipped_dispatch(
                            glyph.destination, scissor,
                            packet.header.surface_width,
                            packet.header.surface_height,
                            &x, &y, &width, &height)) {
                        continue;
                    }
                    VulkanPushConstants constants{};
                    constants.a = {
                        packet.header.surface_width,
                        packet.header.surface_height, 2U, 0U};
                    constants.b = {x, y, width, height};
                    constants.c = {
                        static_cast<std::uint32_t>(glyph.destination.x),
                        static_cast<std::uint32_t>(glyph.destination.y),
                        static_cast<std::uint32_t>(glyph.destination.width),
                        static_cast<std::uint32_t>(glyph.destination.height)};
                    constants.d = {
                        glyph.atlas_page_index,
                        glyph.atlas_x,
                        glyph.atlas_y,
                        glyph.atlas_width};
                    constants.e = {
                        glyph.atlas_height,
                        pack_color(glyph.color),
                        static_cast<std::uint32_t>(glyph.format), 0U};
                    dispatch_locked(constants);
                }
            } else {
                return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                            "Vulkan shader command kind is invalid");
            }
        }

        if (!capture_readback) {
            return true;
        }

        const VkImageMemoryBarrier to_copy = image_barrier(
            output_.handle, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 1U);
        vkCmdPipelineBarrier(
            command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0U,
            0U, nullptr, 0U, nullptr, 1U, &to_copy);
        output_.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkBufferImageCopy copy{};
        copy.bufferOffset = 0U;
        copy.bufferRowLength = 0U;
        copy.bufferImageHeight = 0U;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = 0U;
        copy.imageSubresource.baseArrayLayer = 0U;
        copy.imageSubresource.layerCount = 1U;
        copy.imageExtent = {
            packet.header.surface_width,
            packet.header.surface_height, 1U};
        vkCmdCopyImageToBuffer(
            command_buffer_, output_.handle,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            readback_.handle, 1U, &copy);
        VkBufferMemoryBarrier host_barrier{};
        host_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host_barrier.buffer = readback_.handle;
        host_barrier.offset = 0U;
        host_barrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(
            command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0U,
            0U, nullptr, 1U, &host_barrier, 0U, nullptr);
        const VkImageMemoryBarrier back_to_general = image_barrier(
            output_.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, 1U);
        vkCmdPipelineBarrier(
            command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U,
            0U, nullptr, 0U, nullptr, 1U, &back_to_general);
        output_.layout = VK_IMAGE_LAYOUT_GENERAL;
        return true;
    }

    bool publish_readback_locked(
        const GpuShaderPacket& packet,
        ShaderReadback* output,
        NativeShaderExecutionError* error) noexcept {
        const std::uint64_t byte_count =
            static_cast<std::uint64_t>(packet.header.surface_width) *
            packet.header.surface_height * 4U;
        if (readback_.mapped == nullptr || readback_.size < byte_count) {
            return fail(error, NativeShaderExecutionErrorKind::ReadbackFailed,
                        "Vulkan readback buffer is unavailable");
        }
        if (readback_.coherent == 0U) {
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = readback_.memory;
            range.offset = 0U;
            range.size = VK_WHOLE_SIZE;
            const VkResult result = vkInvalidateMappedMemoryRanges(
                context_->device, 1U, &range);
            if (result != VK_SUCCESS) {
                return fail(error, NativeShaderExecutionErrorKind::ReadbackFailed,
                            "vkInvalidateMappedMemoryRanges failed", result);
            }
        }
        ShaderReadback staged;
        staged.width = packet.header.surface_width;
        staged.height = packet.header.surface_height;
        staged.row_bytes = packet.header.surface_width * 4U;
        staged.bgra.resize(static_cast<std::size_t>(byte_count));
        std::memcpy(
            staged.bgra.data(), readback_.mapped,
            static_cast<std::size_t>(byte_count));
        staged.checksum = shader_bytes_checksum(
            std::span<const std::byte>(staged.bgra));
        *output = std::move(staged);
        return true;
    }

    void destroy_resources_locked() noexcept {
        if (context_ == nullptr || context_->device == VK_NULL_HANDLE) {
            return;
        }
        (void)vkDeviceWaitIdle(context_->device);
        destroy_image(context_->device, &atlas_);
        destroy_image(context_->device, &output_);
        destroy_buffer(context_->device, &readback_);
        if (pipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(context_->device, pipeline_, nullptr);
        }
        if (pipeline_layout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(context_->device, pipeline_layout_, nullptr);
        }
        if (descriptor_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(context_->device, descriptor_pool_, nullptr);
        }
        if (descriptor_set_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(
                context_->device, descriptor_set_layout_, nullptr);
        }
        if (fence_ != VK_NULL_HANDLE) {
            vkDestroyFence(context_->device, fence_, nullptr);
        }
        if (command_pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(context_->device, command_pool_, nullptr);
        }
        pipeline_ = VK_NULL_HANDLE;
        pipeline_layout_ = VK_NULL_HANDLE;
        descriptor_pool_ = VK_NULL_HANDLE;
        descriptor_set_layout_ = VK_NULL_HANDLE;
        descriptor_set_ = VK_NULL_HANDLE;
        fence_ = VK_NULL_HANDLE;
        command_pool_ = VK_NULL_HANDLE;
        command_buffer_ = VK_NULL_HANDLE;
        atlas_signature_ = 0U;
    }

    void reset_locked() noexcept {
        VulkanWsiSharedContext* released = context_;
        if (released != nullptr) {
            {
                std::lock_guard<std::mutex> device_lock(released->device_mutex);
                destroy_resources_locked();
            }
            context_ = nullptr;
            detail::release_vulkan_wsi_context(released);
        }
        config_ = {};
        snapshot_ = {};
        output_generation_ = 0U;
        next_output_generation_ = 1U;
        last_surface_ = {};
    }

    mutable std::mutex mutex_;
    VulkanWsiSharedContext* context_{nullptr};
    NativeShaderExecutionConfig config_{};
    NativeShaderExecutionSnapshot snapshot_{};
    VkCommandPool command_pool_{VK_NULL_HANDLE};
    VkCommandBuffer command_buffer_{VK_NULL_HANDLE};
    VkFence fence_{VK_NULL_HANDLE};
    VkDescriptorSetLayout descriptor_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool_{VK_NULL_HANDLE};
    VkDescriptorSet descriptor_set_{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VulkanImage atlas_{};
    VulkanImage output_{};
    VulkanBuffer readback_{};
    std::uint64_t atlas_signature_{0U};
    std::uint64_t output_generation_{0U};
    std::uint64_t next_output_generation_{1U};
    NativeShaderSurfaceView last_surface_{};
};

} // namespace

std::unique_ptr<NativeShaderExecutor>
make_vulkan_native_shader_executor() noexcept {
    return std::unique_ptr<NativeShaderExecutor>(
        new (std::nothrow) VulkanShaderExecutor());
}

} // namespace zevryon::text

#endif