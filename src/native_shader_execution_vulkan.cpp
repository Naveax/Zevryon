#include "native_shader_execution.hpp"

#if defined(ZEVRYON_HAS_VULKAN_NATIVE_SHADER)

#include "native_shader_execution_vulkan_spirv.hpp"
#include "native_vulkan_wsi_context.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace zevryon::text {
namespace {

constexpr std::uint64_t kFenceTimeoutNanoseconds = 5'000'000'000ULL;

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

struct BufferSlot final {
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkDeviceSize capacity{0U};
    VkMemoryPropertyFlags properties{0U};
};

struct AtlasImage final {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::uint32_t layers{0U};
    std::uint64_t bytes{0U};
};

bool checked_multiply(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t* output) noexcept {
    if (output == nullptr ||
        (left != 0U && right > (std::numeric_limits<std::uint64_t>::max)() / left)) {
        return false;
    }
    *output = left * right;
    return true;
}

class VulkanNativeShaderExecutionApi final : public NativeShaderExecutionApi {
public:
    VulkanNativeShaderExecutionApi() noexcept = default;
    ~VulkanNativeShaderExecutionApi() override { shutdown(); }

    NativeGpuApiKind kind() const noexcept override {
        return NativeGpuApiKind::Vulkan;
    }

    NativeShaderCapabilities capabilities() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        NativeShaderCapabilities result =
            default_native_shader_capabilities(NativeGpuApiKind::Vulkan);
        if (context_ != nullptr && context_->software_device != 0U) {
            result.flags |= kNativeShaderSoftwareDevice;
        }
        return result;
    }

    bool configure(
        const NativeGpuSdkContextHandle& context,
        const NativeShaderExecutionLimits& limits,
        NativeShaderExecutionError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        shutdown_locked();
        if (context.api_kind != NativeGpuApiKind::Vulkan ||
            context.device_generation == 0U || context.runtime_generation == 0U ||
            limits.maximum_atlas_pages == 0U ||
            limits.maximum_atlas_pages > atlas_generations_.size() ||
            limits.maximum_frames_in_flight == 0U ||
            limits.maximum_output_bytes == 0U) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "invalid Vulkan native shader configuration");
        }
        detail::VulkanWsiSharedContext* retained =
            detail::retain_vulkan_wsi_context(context);
        if (retained == nullptr || retained->device == VK_NULL_HANDLE ||
            retained->physical_device == VK_NULL_HANDLE ||
            retained->graphics_queue == VK_NULL_HANDLE) {
            if (retained != nullptr) {
                detail::release_vulkan_wsi_context(retained);
            }
            return fail(error, NativeShaderExecutionErrorKind::StaleGeneration,
                        "Vulkan native shader context could not be retained");
        }
        context_ = retained;
        limits_ = limits;
        bool created = false;
        {
            std::lock_guard<std::mutex> device_lock(context_->device_mutex);
            created = create_pipeline_locked(error) &&
                create_command_resources_locked(error) &&
                create_descriptor_resources_locked(error);
            if (!created) {
                shutdown_resources_locked();
            }
        }
        if (!created) {
            detail::VulkanWsiSharedContext* failed_context = context_;
            context_ = nullptr;
            detail::release_vulkan_wsi_context(failed_context);
            return false;
        }
        snapshot_ = {};
        snapshot_.capabilities =
            default_native_shader_capabilities(NativeGpuApiKind::Vulkan);
        if (context_->software_device != 0U) {
            snapshot_.capabilities.flags |= kNativeShaderSoftwareDevice;
        }
        snapshot_.limits = limits;
        snapshot_.context = context;
        snapshot_.configurations = 1U;
        atlas_generations_.fill(0U);
        next_fence_value_ = 1U;
        configured_ = true;
        return true;
    }

    bool execute(
        const NativeShaderExecutionRequest& request,
        ShaderReadback* readback,
        NativeShaderExecutionReceipt* receipt,
        NativeShaderExecutionError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (!configured_ || context_ == nullptr || request.packet == nullptr ||
            request.atlas == nullptr || receipt == nullptr || request.ticket_id == 0U) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "invalid Vulkan native shader execution request");
        }
        NativeShaderDispatchPlan plan;
        if (!compile_native_shader_dispatch_plan(
                NativeGpuApiKind::Vulkan,
                *request.packet,
                *request.atlas,
                limits_,
                &plan,
                error)) {
            return false;
        }

        std::lock_guard<std::mutex> device_lock(context_->device_mutex);
        if (!ensure_resources_locked(plan, error)) {
            return false;
        }
        if (!write_buffer_locked(constants_, &plan.constants,
                                 sizeof(plan.constants), error) ||
            !write_buffer_locked(commands_, plan.commands.data(),
                                 plan.header.command_bytes, error) ||
            !write_buffer_locked(fills_, plan.fills.data(),
                                 plan.header.fill_bytes, error) ||
            !write_buffer_locked(glyphs_, plan.glyphs.data(),
                                 plan.header.glyph_bytes, error) ||
            !write_buffer_locked(scissors_, plan.scissors.data(),
                                 plan.header.scissor_bytes, error)) {
            return false;
        }

        VkResult result = vkResetCommandPool(
            context_->device, command_pool_, 0U);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                        "vkResetCommandPool failed for native shader execution", result);
        }
        VkCommandBufferBeginInfo begin_info{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command_buffer_, &begin_info);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                        "vkBeginCommandBuffer failed for native shader execution", result);
        }

        std::vector<BufferSlot> atlas_staging;
        std::array<std::uint32_t, 16U> pending_generations = atlas_generations_;
        std::uint64_t pending_upload_count = 0U;
        const VkImageLayout atlas_layout_before = atlas_.layout;
        if (!upload_atlas_locked(
                plan, *request.atlas, &atlas_staging,
                &pending_generations, &pending_upload_count, error)) {
            (void)vkEndCommandBuffer(command_buffer_);
            atlas_.layout = atlas_layout_before;
            destroy_buffer_vector_locked(&atlas_staging);
            return false;
        }
        update_descriptors_locked(plan);
        vkCmdBindPipeline(
            command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(
            command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_,
            0U, 1U, &descriptor_set_, 0U, nullptr);
        vkCmdDispatch(
            command_buffer_, plan.header.dispatch_x,
            plan.header.dispatch_y,
            plan.header.dispatch_z);

        VkBufferMemoryBarrier output_barrier{
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        output_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        output_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        output_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        output_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        output_barrier.buffer = output_.buffer;
        output_barrier.offset = 0U;
        output_barrier.size = plan.header.output_bytes;
        vkCmdPipelineBarrier(
            command_buffer_,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,
            0U,
            0U, nullptr,
            1U, &output_barrier,
            0U, nullptr);

        result = vkEndCommandBuffer(command_buffer_);
        if (result != VK_SUCCESS) {
            atlas_.layout = atlas_layout_before;
            destroy_buffer_vector_locked(&atlas_staging);
            return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                        "vkEndCommandBuffer failed for native shader execution", result);
        }
        result = vkResetFences(context_->device, 1U, &fence_);
        if (result != VK_SUCCESS) {
            atlas_.layout = atlas_layout_before;
            destroy_buffer_vector_locked(&atlas_staging);
            return fail(error, NativeShaderExecutionErrorKind::SubmissionFailed,
                        "vkResetFences failed for native shader execution", result);
        }
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1U;
        submit.pCommandBuffers = &command_buffer_;
        result = vkQueueSubmit(context_->graphics_queue, 1U, &submit, fence_);
        if (result != VK_SUCCESS) {
            atlas_.layout = atlas_layout_before;
            destroy_buffer_vector_locked(&atlas_staging);
            snapshot_.device_lost_events += 1U;
            return fail(error, NativeShaderExecutionErrorKind::DeviceLost,
                        "vkQueueSubmit failed for native shader execution", result);
        }
        result = vkWaitForFences(
            context_->device, 1U, &fence_, VK_TRUE,
            kFenceTimeoutNanoseconds);
        destroy_buffer_vector_locked(&atlas_staging);
        if (result != VK_SUCCESS) {
            if (result == VK_ERROR_DEVICE_LOST) {
                snapshot_.device_lost_events += 1U;
                return fail(error, NativeShaderExecutionErrorKind::DeviceLost,
                            "Vulkan device was lost during shader execution", result);
            }
            return fail(error, NativeShaderExecutionErrorKind::SubmissionFailed,
                        "vkWaitForFences failed for native shader execution", result);
        }
        atlas_generations_ = pending_generations;
        snapshot_.atlas_uploads += pending_upload_count;

        if (!invalidate_buffer_locked(output_, 0U, plan.header.output_bytes, error)) {
            return false;
        }
        void* mapped = nullptr;
        result = vkMapMemory(
            context_->device, output_.memory, 0U,
            plan.header.output_bytes, 0U, &mapped);
        if (result != VK_SUCCESS || mapped == nullptr) {
            return fail(error, NativeShaderExecutionErrorKind::ReadbackFailed,
                        "vkMapMemory failed for shader output readback", result);
        }
        const std::span<const std::byte> bytes(
            static_cast<const std::byte*>(mapped),
            static_cast<std::size_t>(plan.header.output_bytes));
        const std::uint64_t checksum = shader_bytes_checksum(bytes);
        if (readback != nullptr) {
            try {
                ShaderReadback candidate;
                candidate.width = request.packet->header.surface_width;
                candidate.height = request.packet->header.surface_height;
                candidate.row_bytes = candidate.width * 4U;
                candidate.checksum = checksum;
                candidate.bgra.assign(bytes.begin(), bytes.end());
                *readback = std::move(candidate);
            } catch (const std::bad_alloc&) {
                vkUnmapMemory(context_->device, output_.memory);
                return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                            "Vulkan shader readback allocation failed");
            } catch (...) {
                vkUnmapMemory(context_->device, output_.memory);
                return fail(error, NativeShaderExecutionErrorKind::ReadbackFailed,
                            "unexpected Vulkan shader readback failure");
            }
        }
        vkUnmapMemory(context_->device, output_.memory);

        if ((request.flags & kNativeShaderExecutionRequireExactReadback) != 0U &&
            request.expected_readback_checksum != 0U &&
            checksum != request.expected_readback_checksum) {
            return fail(error, NativeShaderExecutionErrorKind::ReadbackMismatch,
                        "Vulkan shader readback differs from the reference oracle");
        }

        const std::uint64_t signal = next_fence_value_++;
        *receipt = {};
        receipt->api_kind = NativeGpuApiKind::Vulkan;
        receipt->flags = request.flags;
        receipt->command_count = static_cast<std::uint32_t>(plan.commands.size());
        receipt->fill_instance_count = static_cast<std::uint32_t>(plan.fills.size());
        receipt->glyph_instance_count = static_cast<std::uint32_t>(plan.glyphs.size());
        receipt->atlas_binding_count =
            static_cast<std::uint32_t>(plan.atlas_bindings.size());
        receipt->dispatch_x = plan.header.dispatch_x;
        receipt->dispatch_y = plan.header.dispatch_y;
        receipt->frame_id = request.packet->header.frame_id;
        receipt->ticket_id = request.ticket_id;
        receipt->wait_fence_value = request.wait_fence_value;
        receipt->signal_fence_value = signal;
        receipt->packet_checksum = request.packet->header.packet_checksum;
        receipt->plan_checksum = plan.header.plan_checksum;
        receipt->readback_checksum = checksum;
        receipt->output_bytes = plan.header.output_bytes;

        snapshot_.executions += 1U;
        snapshot_.readbacks += 1U;
        snapshot_.last_submitted_fence_value = signal;
        snapshot_.completed_fence_value = signal;
        snapshot_.resident_atlas_pages =
            static_cast<std::uint32_t>(plan.atlas_bindings.size());
        if (plan.header.output_bytes >
            (std::numeric_limits<std::uint64_t>::max)() - atlas_.bytes) {
            snapshot_.current_device_bytes =
                (std::numeric_limits<std::uint64_t>::max)();
        } else {
            snapshot_.current_device_bytes =
                plan.header.output_bytes + atlas_.bytes;
        }
        snapshot_.peak_device_bytes = std::max(
            snapshot_.peak_device_bytes, snapshot_.current_device_bytes);
        snapshot_.current_staging_bytes = 0U;
        const std::array<std::uint64_t, 4U> staging_parts{
            plan.header.command_bytes,
            plan.header.fill_bytes,
            plan.header.glyph_bytes,
            plan.header.scissor_bytes};
        for (const std::uint64_t part : staging_parts) {
            if (part > (std::numeric_limits<std::uint64_t>::max)() -
                    snapshot_.current_staging_bytes) {
                snapshot_.current_staging_bytes =
                    (std::numeric_limits<std::uint64_t>::max)();
                break;
            }
            snapshot_.current_staging_bytes += part;
        }
        snapshot_.peak_staging_bytes = std::max(
            snapshot_.peak_staging_bytes, snapshot_.current_staging_bytes);
        return true;
    }

    bool retire_completed(
        std::uint64_t completed_fence_value,
        NativeShaderExecutionError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (!configured_ || completed_fence_value < snapshot_.completed_fence_value ||
            completed_fence_value > snapshot_.last_submitted_fence_value) {
            return fail(error, NativeShaderExecutionErrorKind::StaleGeneration,
                        "Vulkan shader completion fence is outside the submitted timeline");
        }
        snapshot_.completed_fence_value = completed_fence_value;
        return true;
    }

    NativeShaderExecutionSnapshot snapshot() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

    void shutdown() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_locked();
    }

private:
    bool find_memory_type_locked(
        std::uint32_t type_bits,
        VkMemoryPropertyFlags required,
        std::uint32_t* type_index,
        VkMemoryPropertyFlags* actual) const noexcept {
        if (type_index == nullptr || actual == nullptr || context_ == nullptr) {
            return false;
        }
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(context_->physical_device, &properties);
        for (std::uint32_t index = 0U;
             index < properties.memoryTypeCount; ++index) {
            if ((type_bits & (1U << index)) != 0U &&
                (properties.memoryTypes[index].propertyFlags & required) == required) {
                *type_index = index;
                *actual = properties.memoryTypes[index].propertyFlags;
                return true;
            }
        }
        return false;
    }

    bool create_buffer_locked(
        VkDeviceSize bytes,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags required,
        BufferSlot* output,
        NativeShaderExecutionError* error) noexcept {
        if (output == nullptr || bytes == 0U) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "invalid Vulkan shader buffer request");
        }
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = bytes;
        buffer_info.usage = usage;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkResult result = vkCreateBuffer(
            context_->device, &buffer_info, nullptr, &buffer);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "vkCreateBuffer failed for native shader resources", result);
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(context_->device, buffer, &requirements);
        std::uint32_t memory_type = 0U;
        VkMemoryPropertyFlags actual = 0U;
        if (!find_memory_type_locked(
                requirements.memoryTypeBits, required, &memory_type, &actual)) {
            vkDestroyBuffer(context_->device, buffer, nullptr);
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "no Vulkan memory type satisfies native shader buffer requirements");
        }
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        result = vkAllocateMemory(context_->device, &allocation, nullptr, &memory);
        if (result != VK_SUCCESS) {
            vkDestroyBuffer(context_->device, buffer, nullptr);
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "vkAllocateMemory failed for native shader buffer", result);
        }
        result = vkBindBufferMemory(context_->device, buffer, memory, 0U);
        if (result != VK_SUCCESS) {
            vkFreeMemory(context_->device, memory, nullptr);
            vkDestroyBuffer(context_->device, buffer, nullptr);
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "vkBindBufferMemory failed for native shader buffer", result);
        }
        output->buffer = buffer;
        output->memory = memory;
        output->capacity = requirements.size;
        output->properties = actual;
        return true;
    }

    void destroy_buffer_locked(BufferSlot* slot) noexcept {
        if (slot == nullptr || context_ == nullptr) {
            return;
        }
        if (slot->buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(context_->device, slot->buffer, nullptr);
        }
        if (slot->memory != VK_NULL_HANDLE) {
            vkFreeMemory(context_->device, slot->memory, nullptr);
        }
        *slot = {};
    }

    void destroy_buffer_vector_locked(std::vector<BufferSlot>* slots) noexcept {
        if (slots == nullptr) {
            return;
        }
        for (BufferSlot& slot : *slots) {
            destroy_buffer_locked(&slot);
        }
        slots->clear();
    }

    bool ensure_buffer_locked(
        VkDeviceSize bytes,
        VkBufferUsageFlags usage,
        BufferSlot* slot,
        NativeShaderExecutionError* error) noexcept {
        const VkDeviceSize required = std::max<VkDeviceSize>(bytes, 16U);
        if (slot->buffer != VK_NULL_HANDLE && slot->capacity >= required) {
            return true;
        }
        BufferSlot replacement;
        if (!create_buffer_locked(
                required,
                usage,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &replacement,
                error)) {
            return false;
        }
        destroy_buffer_locked(slot);
        *slot = replacement;
        return true;
    }

    bool write_buffer_locked(
        const BufferSlot& slot,
        const void* source,
        std::uint64_t bytes,
        NativeShaderExecutionError* error) noexcept {
        if (bytes == 0U) {
            return true;
        }
        if (slot.memory == VK_NULL_HANDLE || source == nullptr ||
            bytes > slot.capacity) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "invalid Vulkan shader buffer upload");
        }
        void* mapped = nullptr;
        const VkResult result = vkMapMemory(
            context_->device, slot.memory, 0U, bytes, 0U, &mapped);
        if (result != VK_SUCCESS || mapped == nullptr) {
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "vkMapMemory failed for native shader upload", result);
        }
        std::memcpy(mapped, source, static_cast<std::size_t>(bytes));
        if ((slot.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0U) {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = slot.memory;
            range.offset = 0U;
            range.size = bytes;
            const VkResult flushed = vkFlushMappedMemoryRanges(
                context_->device, 1U, &range);
            if (flushed != VK_SUCCESS) {
                vkUnmapMemory(context_->device, slot.memory);
                return fail(error, NativeShaderExecutionErrorKind::SubmissionFailed,
                            "vkFlushMappedMemoryRanges failed for shader upload", flushed);
            }
        }
        vkUnmapMemory(context_->device, slot.memory);
        return true;
    }

    bool invalidate_buffer_locked(
        const BufferSlot& slot,
        VkDeviceSize offset,
        VkDeviceSize bytes,
        NativeShaderExecutionError* error) noexcept {
        if ((slot.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U) {
            return true;
        }
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = slot.memory;
        range.offset = offset;
        range.size = bytes;
        const VkResult result = vkInvalidateMappedMemoryRanges(
            context_->device, 1U, &range);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::ReadbackFailed,
                        "vkInvalidateMappedMemoryRanges failed for shader output", result);
        }
        return true;
    }

    bool create_pipeline_locked(
        NativeShaderExecutionError* error) noexcept {
        VkShaderModuleCreateInfo module_info{
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_info.codeSize =
            detail::kNativeShaderVulkanSpirvWordCount * sizeof(std::uint32_t);
        module_info.pCode = detail::kNativeShaderVulkanSpirv;
        VkResult result = vkCreateShaderModule(
            context_->device, &module_info, nullptr, &shader_module_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::ShaderCompilationFailed,
                        "vkCreateShaderModule failed for native shader pipeline", result);
        }

        std::array<VkDescriptorSetLayoutBinding, 7U> bindings{};
        bindings[0] = VkDescriptorSetLayoutBinding{
            0U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1U,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        for (std::uint32_t index = 1U; index <= 4U; ++index) {
            bindings[index] = VkDescriptorSetLayoutBinding{
                index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        }
        bindings[5] = VkDescriptorSetLayoutBinding{
            5U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1U,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[6] = VkDescriptorSetLayoutBinding{
            6U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo layout_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layout_info.pBindings = bindings.data();
        result = vkCreateDescriptorSetLayout(
            context_->device, &layout_info, nullptr, &descriptor_set_layout_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "vkCreateDescriptorSetLayout failed for native shader pipeline", result);
        }
        VkPipelineLayoutCreateInfo pipeline_layout_info{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeline_layout_info.setLayoutCount = 1U;
        pipeline_layout_info.pSetLayouts = &descriptor_set_layout_;
        result = vkCreatePipelineLayout(
            context_->device, &pipeline_layout_info, nullptr, &pipeline_layout_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "vkCreatePipelineLayout failed for native shader pipeline", result);
        }
        VkPipelineShaderStageCreateInfo stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader_module_;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipeline_info{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_info.stage = stage;
        pipeline_info.layout = pipeline_layout_;
        result = vkCreateComputePipelines(
            context_->device, VK_NULL_HANDLE, 1U,
            &pipeline_info, nullptr, &pipeline_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "vkCreateComputePipelines failed for native shader pipeline", result);
        }
        return true;
    }

    bool create_command_resources_locked(
        NativeShaderExecutionError* error) noexcept {
        VkCommandPoolCreateInfo pool_info{
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = context_->graphics_queue_family;
        VkResult result = vkCreateCommandPool(
            context_->device, &pool_info, nullptr, &command_pool_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "vkCreateCommandPool failed for native shader execution", result);
        }
        VkCommandBufferAllocateInfo allocation{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = command_pool_;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1U;
        result = vkAllocateCommandBuffers(
            context_->device, &allocation, &command_buffer_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "vkAllocateCommandBuffers failed for native shader execution", result);
        }
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result = vkCreateFence(
            context_->device, &fence_info, nullptr, &fence_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "vkCreateFence failed for native shader execution", result);
        }
        VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler_info.magFilter = VK_FILTER_NEAREST;
        sampler_info.minFilter = VK_FILTER_NEAREST;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.maxLod = 0.0F;
        result = vkCreateSampler(
            context_->device, &sampler_info, nullptr, &sampler_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "vkCreateSampler failed for native shader atlas", result);
        }
        return true;
    }

    bool create_descriptor_resources_locked(
        NativeShaderExecutionError* error) noexcept {
        const std::array<VkDescriptorPoolSize, 3U> sizes{{
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1U},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5U},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1U}}};
        VkDescriptorPoolCreateInfo pool_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.maxSets = 1U;
        pool_info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        pool_info.pPoolSizes = sizes.data();
        VkResult result = vkCreateDescriptorPool(
            context_->device, &pool_info, nullptr, &descriptor_pool_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "vkCreateDescriptorPool failed for native shader execution", result);
        }
        VkDescriptorSetAllocateInfo allocation{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = descriptor_pool_;
        allocation.descriptorSetCount = 1U;
        allocation.pSetLayouts = &descriptor_set_layout_;
        result = vkAllocateDescriptorSets(
            context_->device, &allocation, &descriptor_set_);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "vkAllocateDescriptorSets failed for native shader execution", result);
        }
        return true;
    }

    bool ensure_resources_locked(
        const NativeShaderDispatchPlan& plan,
        NativeShaderExecutionError* error) noexcept {
        if (!ensure_buffer_locked(
                sizeof(NativeShaderConstants),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                &constants_, error) ||
            !ensure_buffer_locked(
                plan.header.command_bytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                &commands_, error) ||
            !ensure_buffer_locked(
                plan.header.fill_bytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                &fills_, error) ||
            !ensure_buffer_locked(
                plan.header.glyph_bytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                &glyphs_, error) ||
            !ensure_buffer_locked(
                plan.header.scissor_bytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                &scissors_, error) ||
            !ensure_buffer_locked(
                plan.header.output_bytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                &output_, error)) {
            return false;
        }
        std::uint32_t width = 1U;
        std::uint32_t height = 1U;
        for (const NativeShaderAtlasBinding& binding : plan.atlas_bindings) {
            width = std::max(width, static_cast<std::uint32_t>(binding.width));
            height = std::max(height, static_cast<std::uint32_t>(binding.height));
        }
        if (atlas_.image != VK_NULL_HANDLE && atlas_.width >= width &&
            atlas_.height >= height && atlas_.layers >= limits_.maximum_atlas_pages) {
            return true;
        }
        AtlasImage replacement;
        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UINT;
        image_info.extent = VkExtent3D{width, height, 1U};
        image_info.mipLevels = 1U;
        image_info.arrayLayers = limits_.maximum_atlas_pages;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult result = vkCreateImage(
            context_->device, &image_info, nullptr, &replacement.image);
        if (result != VK_SUCCESS) {
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "vkCreateImage failed for native shader atlas", result);
        }
        auto discard_replacement = [&]() noexcept {
            if (replacement.view != VK_NULL_HANDLE) {
                vkDestroyImageView(context_->device, replacement.view, nullptr);
            }
            if (replacement.image != VK_NULL_HANDLE) {
                vkDestroyImage(context_->device, replacement.image, nullptr);
            }
            if (replacement.memory != VK_NULL_HANDLE) {
                vkFreeMemory(context_->device, replacement.memory, nullptr);
            }
            replacement = {};
        };
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(
            context_->device, replacement.image, &requirements);
        std::uint32_t memory_type = 0U;
        VkMemoryPropertyFlags actual = 0U;
        if (!find_memory_type_locked(
                requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                &memory_type,
                &actual)) {
            discard_replacement();
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "no device-local Vulkan memory type for shader atlas");
        }
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type;
        result = vkAllocateMemory(
            context_->device, &allocation, nullptr, &replacement.memory);
        if (result != VK_SUCCESS) {
            discard_replacement();
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "vkAllocateMemory failed for native shader atlas", result);
        }
        result = vkBindImageMemory(
            context_->device, replacement.image, replacement.memory, 0U);
        if (result != VK_SUCCESS) {
            discard_replacement();
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "vkBindImageMemory failed for native shader atlas", result);
        }
        VkImageViewCreateInfo view_info{
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = replacement.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view_info.format = VK_FORMAT_R8G8B8A8_UINT;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0U;
        view_info.subresourceRange.levelCount = 1U;
        view_info.subresourceRange.baseArrayLayer = 0U;
        view_info.subresourceRange.layerCount = limits_.maximum_atlas_pages;
        result = vkCreateImageView(
            context_->device, &view_info, nullptr, &replacement.view);
        if (result != VK_SUCCESS) {
            discard_replacement();
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "vkCreateImageView failed for native shader atlas", result);
        }
        replacement.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        replacement.width = width;
        replacement.height = height;
        replacement.layers = limits_.maximum_atlas_pages;
        std::uint64_t pixels = 0U;
        if (!checked_multiply(width, height, &pixels) ||
            !checked_multiply(pixels, replacement.layers, &pixels) ||
            !checked_multiply(pixels, 4U, &replacement.bytes)) {
            discard_replacement();
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "Vulkan atlas byte count overflow");
        }
        destroy_atlas_locked();
        atlas_ = replacement;
        replacement = {};
        atlas_generations_.fill(0U);
        return true;
    }

    void transition_atlas_locked(
        VkImageLayout new_layout,
        VkPipelineStageFlags source_stage,
        VkPipelineStageFlags destination_stage,
        VkAccessFlags source_access,
        VkAccessFlags destination_access) noexcept {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = source_access;
        barrier.dstAccessMask = destination_access;
        barrier.oldLayout = atlas_.layout;
        barrier.newLayout = new_layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = atlas_.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0U;
        barrier.subresourceRange.levelCount = 1U;
        barrier.subresourceRange.baseArrayLayer = 0U;
        barrier.subresourceRange.layerCount = atlas_.layers;
        vkCmdPipelineBarrier(
            command_buffer_, source_stage, destination_stage, 0U,
            0U, nullptr, 0U, nullptr, 1U, &barrier);
        atlas_.layout = new_layout;
    }

    bool upload_atlas_locked(
        const NativeShaderDispatchPlan& plan,
        const ShaderAtlasResidency& atlas,
        std::vector<BufferSlot>* staging,
        std::array<std::uint32_t, 16U>* pending_generations,
        std::uint64_t* pending_upload_count,
        NativeShaderExecutionError* error) noexcept {
        if (staging == nullptr || pending_generations == nullptr ||
            pending_upload_count == nullptr) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "null Vulkan atlas publication target");
        }
        bool needs_upload = false;
        for (const NativeShaderAtlasBinding& binding : plan.atlas_bindings) {
            if (binding.texture_layer >= atlas_generations_.size()) {
                return fail(error, NativeShaderExecutionErrorKind::InvalidAtlasReference,
                            "Vulkan atlas texture layer exceeds the certified limit");
            }
            if ((*pending_generations)[binding.texture_layer] != binding.page_generation) {
                needs_upload = true;
                break;
            }
        }
        if (!needs_upload) {
            return true;
        }
        const VkImageLayout previous_layout = atlas_.layout;
        transition_atlas_locked(
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            previous_layout == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            previous_layout == VK_IMAGE_LAYOUT_UNDEFINED
                ? 0U : VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT);

        try {
            for (const NativeShaderAtlasBinding& binding : plan.atlas_bindings) {
                if ((*pending_generations)[binding.texture_layer] == binding.page_generation) {
                    continue;
                }
                const ShaderAtlasResidentPage* page = atlas.find(
                    binding.page_index, binding.page_generation);
                if (page == nullptr ||
                    page->canonical_bgra.size() != binding.resident_bytes) {
                    return fail(error, NativeShaderExecutionErrorKind::InvalidAtlasReference,
                                "Vulkan shader atlas page is no longer resident");
                }
                BufferSlot upload;
                if (!create_buffer_locked(
                        binding.resident_bytes,
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &upload,
                        error)) {
                    return false;
                }
                if (!write_buffer_locked(
                        upload, page->canonical_bgra.data(),
                        binding.resident_bytes, error)) {
                    destroy_buffer_locked(&upload);
                    return false;
                }
                VkBufferImageCopy copy{};
                copy.bufferOffset = 0U;
                copy.bufferRowLength = 0U;
                copy.bufferImageHeight = 0U;
                copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                copy.imageSubresource.mipLevel = 0U;
                copy.imageSubresource.baseArrayLayer = binding.texture_layer;
                copy.imageSubresource.layerCount = 1U;
                copy.imageOffset = VkOffset3D{0, 0, 0};
                copy.imageExtent = VkExtent3D{
                    binding.width, binding.height, 1U};
                vkCmdCopyBufferToImage(
                    command_buffer_, upload.buffer, atlas_.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1U, &copy);
                try {
                    staging->push_back(upload);
                } catch (const std::bad_alloc&) {
                    destroy_buffer_locked(&upload);
                    return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                                "Vulkan atlas keep-alive allocation failed");
                } catch (...) {
                    destroy_buffer_locked(&upload);
                    return fail(error, NativeShaderExecutionErrorKind::SubmissionFailed,
                                "unexpected Vulkan atlas keep-alive failure");
                }
                (*pending_generations)[binding.texture_layer] = binding.page_generation;
                *pending_upload_count += 1U;
            }
        } catch (const std::bad_alloc&) {
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "Vulkan atlas upload keep-alive allocation failed");
        } catch (...) {
            return fail(error, NativeShaderExecutionErrorKind::SubmissionFailed,
                        "unexpected Vulkan atlas upload failure");
        }
        transition_atlas_locked(
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT);
        return true;
    }

    void update_descriptors_locked(
        const NativeShaderDispatchPlan& plan) noexcept {
        const std::array<VkDescriptorBufferInfo, 6U> buffers{{
            {constants_.buffer, 0U, sizeof(NativeShaderConstants)},
            {commands_.buffer, 0U, std::max<std::uint64_t>(16U, plan.header.command_bytes)},
            {fills_.buffer, 0U, std::max<std::uint64_t>(16U, plan.header.fill_bytes)},
            {glyphs_.buffer, 0U, std::max<std::uint64_t>(16U, plan.header.glyph_bytes)},
            {scissors_.buffer, 0U, std::max<std::uint64_t>(16U, plan.header.scissor_bytes)},
            {output_.buffer, 0U, plan.header.output_bytes}}};
        VkDescriptorImageInfo image{};
        image.sampler = sampler_;
        image.imageView = atlas_.view;
        image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        std::array<VkWriteDescriptorSet, 7U> writes{};
        for (std::uint32_t index = 0U; index < 5U; ++index) {
            writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index].dstSet = descriptor_set_;
            writes[index].dstBinding = index;
            writes[index].descriptorCount = 1U;
            writes[index].descriptorType = index == 0U
                ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[index].pBufferInfo = &buffers[index];
        }
        writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[5].dstSet = descriptor_set_;
        writes[5].dstBinding = 5U;
        writes[5].descriptorCount = 1U;
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[5].pImageInfo = &image;
        writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[6].dstSet = descriptor_set_;
        writes[6].dstBinding = 6U;
        writes[6].descriptorCount = 1U;
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[6].pBufferInfo = &buffers[5];
        vkUpdateDescriptorSets(
            context_->device,
            static_cast<std::uint32_t>(writes.size()), writes.data(),
            0U, nullptr);
    }

    void destroy_atlas_locked() noexcept {
        if (context_ == nullptr) {
            atlas_ = {};
            return;
        }
        if (atlas_.view != VK_NULL_HANDLE) {
            vkDestroyImageView(context_->device, atlas_.view, nullptr);
        }
        if (atlas_.image != VK_NULL_HANDLE) {
            vkDestroyImage(context_->device, atlas_.image, nullptr);
        }
        if (atlas_.memory != VK_NULL_HANDLE) {
            vkFreeMemory(context_->device, atlas_.memory, nullptr);
        }
        atlas_ = {};
        atlas_generations_.fill(0U);
    }

    void shutdown_resources_locked() noexcept {
        if (context_ == nullptr) {
            return;
        }
        (void)vkDeviceWaitIdle(context_->device);
        destroy_buffer_locked(&constants_);
        destroy_buffer_locked(&commands_);
        destroy_buffer_locked(&fills_);
        destroy_buffer_locked(&glyphs_);
        destroy_buffer_locked(&scissors_);
        destroy_buffer_locked(&output_);
        destroy_atlas_locked();
        if (sampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(context_->device, sampler_, nullptr);
            sampler_ = VK_NULL_HANDLE;
        }
        if (descriptor_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(context_->device, descriptor_pool_, nullptr);
            descriptor_pool_ = VK_NULL_HANDLE;
            descriptor_set_ = VK_NULL_HANDLE;
        }
        if (fence_ != VK_NULL_HANDLE) {
            vkDestroyFence(context_->device, fence_, nullptr);
            fence_ = VK_NULL_HANDLE;
        }
        if (command_pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(context_->device, command_pool_, nullptr);
            command_pool_ = VK_NULL_HANDLE;
            command_buffer_ = VK_NULL_HANDLE;
        }
        if (pipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(context_->device, pipeline_, nullptr);
            pipeline_ = VK_NULL_HANDLE;
        }
        if (pipeline_layout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(context_->device, pipeline_layout_, nullptr);
            pipeline_layout_ = VK_NULL_HANDLE;
        }
        if (descriptor_set_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(
                context_->device, descriptor_set_layout_, nullptr);
            descriptor_set_layout_ = VK_NULL_HANDLE;
        }
        if (shader_module_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(context_->device, shader_module_, nullptr);
            shader_module_ = VK_NULL_HANDLE;
        }
    }

    void shutdown_locked() noexcept {
        if (context_ != nullptr) {
            detail::VulkanWsiSharedContext* retained = context_;
            {
                std::lock_guard<std::mutex> device_lock(retained->device_mutex);
                shutdown_resources_locked();
            }
            context_ = nullptr;
            detail::release_vulkan_wsi_context(retained);
        }
        snapshot_ = {};
        limits_ = {};
        configured_ = false;
        next_fence_value_ = 1U;
    }

    mutable std::mutex mutex_;
    detail::VulkanWsiSharedContext* context_{nullptr};
    NativeShaderExecutionLimits limits_{};
    NativeShaderExecutionSnapshot snapshot_{};
    VkDescriptorSetLayout descriptor_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
    VkShaderModule shader_module_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkCommandPool command_pool_{VK_NULL_HANDLE};
    VkCommandBuffer command_buffer_{VK_NULL_HANDLE};
    VkFence fence_{VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool_{VK_NULL_HANDLE};
    VkDescriptorSet descriptor_set_{VK_NULL_HANDLE};
    VkSampler sampler_{VK_NULL_HANDLE};
    BufferSlot constants_{};
    BufferSlot commands_{};
    BufferSlot fills_{};
    BufferSlot glyphs_{};
    BufferSlot scissors_{};
    BufferSlot output_{};
    AtlasImage atlas_{};
    std::array<std::uint32_t, 16U> atlas_generations_{};
    std::uint64_t next_fence_value_{1U};
    bool configured_{false};
};

} // namespace

std::unique_ptr<NativeShaderExecutionApi>
make_vulkan_native_shader_execution_api() noexcept {
    try {
        return std::make_unique<VulkanNativeShaderExecutionApi>();
    } catch (...) {
        return nullptr;
    }
}

} // namespace zevryon::text

#endif // ZEVRYON_HAS_VULKAN_NATIVE_SHADER
