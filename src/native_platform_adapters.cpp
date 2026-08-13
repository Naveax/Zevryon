#include "native_platform_adapters.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace zevryon::text {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::size_t kAdapterScratchBytes = 256U * 1024U;

void hash_bytes(std::uint64_t* hash, const void* data, std::size_t size) noexcept {
    if (hash == nullptr || data == nullptr) {
        return;
    }
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0U; index < size; ++index) {
        *hash ^= static_cast<std::uint64_t>(bytes[index]);
        *hash *= kFnvPrime;
    }
}

template <typename T>
void hash_value(std::uint64_t* hash, const T& value) noexcept {
    hash_bytes(hash, &value, sizeof(value));
}

bool surface_valid(const GpuSurfaceDescriptor& surface) noexcept {
    return surface.surface_id != 0U &&
        surface.generation_id != 0U &&
        surface.width != 0U &&
        surface.height != 0U;
}

bool api_kind_supported(NativeGpuApiKind kind) noexcept {
    return kind == NativeGpuApiKind::Vulkan ||
        kind == NativeGpuApiKind::Direct3D12;
}

bool mode_supported(
    NativePresentMode mode,
    const NativePlatformCapabilities& capabilities) noexcept {
    switch (mode) {
        case NativePresentMode::Fifo:
            return true;
        case NativePresentMode::Mailbox:
            return (capabilities.flags & kNativePlatformMailboxPresent) != 0U;
        case NativePresentMode::Immediate:
            return (capabilities.flags & kNativePlatformImmediatePresent) != 0U;
    }
    return false;
}

void set_compile_error(
    NativePlatformCompileError* error,
    NativePlatformCompileErrorKind kind,
    const char* message,
    std::size_t command_index = 0U,
    std::size_t draw_index = 0U,
    std::uint32_t page_index = 0U) {
    if (error == nullptr) {
        return;
    }
    error->kind = kind;
    error->command_index = command_index;
    error->draw_index = draw_index;
    error->page_index = page_index;
    error->message = message != nullptr ? message : "";
}

void set_api_error(
    NativeGpuApiError* error,
    NativeGpuApiErrorKind kind,
    const char* message) {
    if (error == nullptr) {
        return;
    }
    error->kind = kind;
    error->message = message != nullptr ? message : "";
}

std::uint32_t platform_transition_flags(NativeGpuApiKind kind) noexcept {
    switch (kind) {
        case NativeGpuApiKind::Vulkan:
            return 0x0001U; // image-layout plus access/stage barrier
        case NativeGpuApiKind::Direct3D12:
            return 0x0004U; // D3D12_RESOURCE_BARRIER transition
        case NativeGpuApiKind::Metal:
        case NativeGpuApiKind::ReferenceCpu:
            break;
    }
    return 0U;
}

std::uint32_t present_flags(
    NativeGpuApiKind kind,
    NativePresentMode mode,
    const NativePlatformAdapterConfig& config) noexcept {
    std::uint32_t flags = 0U;
    if (kind == NativeGpuApiKind::Direct3D12 &&
        mode == NativePresentMode::Immediate &&
        (config.flags & kNativePlatformAllowTearing) != 0U) {
        flags |= kNativePlatformCommandAllowTearing;
    }
    return flags;
}

bool append_command(
    NativePlatformSubmission* output,
    const NativePlatformCommandRecord& command,
    std::uint32_t maximum,
    NativePlatformCompileError* error,
    std::size_t source_index) {
    if (output->commands.size() >= maximum) {
        set_compile_error(
            error,
            NativePlatformCompileErrorKind::CommandCapacityExceeded,
            "native platform command capacity exceeded",
            source_index);
        return false;
    }
    output->commands.push_back(command);
    return true;
}

bool append_barrier(
    NativePlatformSubmission* output,
    const NativePlatformBarrierRecord& barrier,
    std::uint32_t maximum,
    NativePlatformCompileError* error,
    std::size_t source_index) {
    if (output->barriers.size() >= maximum) {
        set_compile_error(
            error,
            NativePlatformCompileErrorKind::BarrierCapacityExceeded,
            "native platform barrier capacity exceeded",
            source_index);
        return false;
    }
    output->barriers.push_back(barrier);
    return true;
}

bool find_or_append_descriptor(
    NativePlatformSubmission* output,
    const GpuFrameSubmission& frame,
    const GpuFrameGlyphBatch& batch,
    std::uint32_t maximum,
    std::uint32_t* descriptor_index,
    NativePlatformCompileError* error,
    std::size_t command_index) {
    if (descriptor_index == nullptr || batch.page_reference_index >= frame.page_references.size()) {
        set_compile_error(
            error,
            NativePlatformCompileErrorKind::CommandTopologyViolation,
            "glyph batch references an invalid page",
            command_index,
            0U,
            batch.page_index);
        return false;
    }
    const GpuFramePageReference& page = frame.page_references[batch.page_reference_index];
    for (std::size_t index = 0U; index < output->descriptors.size(); ++index) {
        const NativePlatformDescriptorBinding& candidate = output->descriptors[index];
        if (candidate.atlas_generation_id == frame.atlas_generation_id &&
            candidate.page_generation == page.page_generation &&
            candidate.page_index == page.page_index &&
            candidate.format == page.format) {
            *descriptor_index = static_cast<std::uint32_t>(index);
            return true;
        }
    }
    if (output->descriptors.size() >= maximum) {
        set_compile_error(
            error,
            NativePlatformCompileErrorKind::DescriptorCapacityExceeded,
            "native platform descriptor capacity exceeded",
            command_index,
            0U,
            page.page_index);
        return false;
    }
    NativePlatformDescriptorBinding descriptor;
    descriptor.atlas_generation_id = frame.atlas_generation_id;
    descriptor.page_generation = page.page_generation;
    descriptor.page_index = page.page_index;
    descriptor.descriptor_slot = static_cast<std::uint32_t>(output->descriptors.size());
    descriptor.format = page.format;
    output->descriptors.push_back(descriptor);
    *descriptor_index = descriptor.descriptor_slot;
    return true;
}

std::uint64_t submission_checksum(const NativePlatformSubmission& submission) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_value(&hash, submission.api_kind);
    hash_value(&hash, submission.surface);
    hash_value(&hash, submission.image);
    hash_value(&hash, submission.frame_id);
    hash_value(&hash, submission.ticket_id);
    hash_value(&hash, submission.wait_fence_value);
    hash_value(&hash, submission.command_generation);
    hash_value(&hash, submission.source_command_checksum);
    for (const NativePlatformBarrierRecord& record : submission.barriers) {
        hash_value(&hash, record);
    }
    for (const NativePlatformDescriptorBinding& record : submission.descriptors) {
        hash_value(&hash, record);
    }
    for (const NativePlatformCommandRecord& record : submission.commands) {
        hash_value(&hash, record);
    }
    return hash;
}

} // namespace

NativePlatformSubmission::NativePlatformSubmission(std::pmr::memory_resource* resource)
    : commands(resource), barriers(resource), descriptors(resource) {}

std::pmr::memory_resource* NativePlatformSubmission::resource() const noexcept {
    return commands.get_allocator().resource();
}

void NativePlatformSubmission::release() noexcept {
    commands.clear();
    barriers.clear();
    descriptors.clear();
    api_kind = NativeGpuApiKind::ReferenceCpu;
    surface = {};
    image = {};
    frame_id = 0U;
    ticket_id = 0U;
    wait_fence_value = 0U;
    command_generation = 0U;
    source_command_checksum = 0U;
    encoded_checksum = 0U;
}

const char* native_platform_compile_error_kind_name(
    NativePlatformCompileErrorKind kind) noexcept {
    switch (kind) {
        case NativePlatformCompileErrorKind::None: return "none";
        case NativePlatformCompileErrorKind::InvalidInput: return "invalid-input";
        case NativePlatformCompileErrorKind::UnsupportedCapability: return "unsupported-capability";
        case NativePlatformCompileErrorKind::StaleCommandBuffer: return "stale-command-buffer";
        case NativePlatformCompileErrorKind::StaleSwapchainImage: return "stale-swapchain-image";
        case NativePlatformCompileErrorKind::CommandTopologyViolation: return "command-topology-violation";
        case NativePlatformCompileErrorKind::DrawTopologyViolation: return "draw-topology-violation";
        case NativePlatformCompileErrorKind::UploadFenceNotReady: return "upload-fence-not-ready";
        case NativePlatformCompileErrorKind::CommandCapacityExceeded: return "command-capacity-exceeded";
        case NativePlatformCompileErrorKind::BarrierCapacityExceeded: return "barrier-capacity-exceeded";
        case NativePlatformCompileErrorKind::DescriptorCapacityExceeded: return "descriptor-capacity-exceeded";
        case NativePlatformCompileErrorKind::OutputBudgetExceeded: return "output-budget-exceeded";
        case NativePlatformCompileErrorKind::ArithmeticOverflow: return "arithmetic-overflow";
        case NativePlatformCompileErrorKind::AggregateOverflow: return "aggregate-overflow";
    }
    return "unknown";
}

bool compile_native_platform_submission(
    const NativePlatformCompileRequest& request,
    NativePlatformSubmission* output,
    NativePlatformCompileStats* stats,
    NativePlatformCompileError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
    if (stats != nullptr) {
        *stats = {};
    }
    if (request.config.api_kind == NativeGpuApiKind::Metal) {
        if (output != nullptr) {
            output->release();
        }
        set_compile_error(
            error,
            NativePlatformCompileErrorKind::UnsupportedCapability,
            "Metal support was removed from Zevryon");
        return false;
    }
    if (output == nullptr || request.commands == nullptr || request.frame == nullptr ||
        !api_kind_supported(request.config.api_kind) ||
        request.config.device_generation == 0U ||
        request.config.driver_generation == 0U ||
        request.config.maximum_commands == 0U ||
        request.config.maximum_barriers == 0U ||
        request.config.maximum_descriptors == 0U) {
        set_compile_error(error, NativePlatformCompileErrorKind::InvalidInput, "invalid native platform compile request");
        return false;
    }
    output->release();
    if (!native_command_buffer_is_current(*request.frame, *request.commands)) {
        set_compile_error(error, NativePlatformCompileErrorKind::StaleCommandBuffer, "native command buffer is stale");
        return false;
    }
    if (request.image.image.device_generation != request.config.device_generation ||
        request.image.driver_generation != request.config.driver_generation ||
        request.image.image.surface_id != request.frame->surface.surface_id ||
        request.image.image.surface_generation != request.frame->surface.generation_id ||
        request.image.image.image_generation == 0U ||
        request.image.native_resource_id == 0U ||
        request.image.state != NativePlatformResourceState::Present) {
        set_compile_error(error, NativePlatformCompileErrorKind::StaleSwapchainImage, "swapchain image identity is stale");
        return false;
    }
    if (request.commands->commands.empty() ||
        request.commands->commands.front().kind != NativeCommandKind::BeginRenderPass ||
        request.commands->commands.back().kind != NativeCommandKind::EndRenderPass) {
        set_compile_error(error, NativePlatformCompileErrorKind::CommandTopologyViolation, "native command stream lacks render-pass boundaries");
        return false;
    }

    try {
        NativePlatformSubmission staged(output->resource());
        staged.api_kind = request.config.api_kind;
        staged.surface = request.frame->surface;
        staged.image = request.image;
        staged.frame_id = request.frame->frame_id;
        staged.ticket_id = request.ticket_id;
        staged.wait_fence_value = request.wait_fence_value;
        staged.command_generation = request.commands->command_generation;
        staged.source_command_checksum = request.commands->command_checksum;
        staged.commands.reserve(std::min<std::size_t>(
            request.config.maximum_commands,
            request.commands->commands.size() * 2U + 12U));
        staged.barriers.reserve(std::min<std::size_t>(request.config.maximum_barriers, 2U));
        staged.descriptors.reserve(std::min<std::size_t>(
            request.config.maximum_descriptors,
            request.frame->page_references.size()));

        if (stats != nullptr) {
            stats->input_native_commands = request.commands->commands.size();
            stats->input_damage_rects = request.commands->damage_rects.size();
        }

        if (!append_command(
                &staged,
                {NativePlatformCommandKind::BeginCommandBuffer, 0U, 0U, 0U, request.ticket_id, 0U},
                request.config.maximum_commands,
                error,
                0U)) {
            return false;
        }

        NativePlatformBarrierRecord begin_barrier;
        begin_barrier.resource_id = request.image.native_resource_id;
        begin_barrier.resource_generation = request.image.image.image_generation;
        begin_barrier.before = NativePlatformResourceState::Present;
        begin_barrier.after = NativePlatformResourceState::RenderTarget;
        begin_barrier.flags = platform_transition_flags(request.config.api_kind);
        if (!append_barrier(
                &staged,
                begin_barrier,
                request.config.maximum_barriers,
                error,
                0U) ||
            !append_command(
                &staged,
                {NativePlatformCommandKind::Transition, 0U, 0U, begin_barrier.flags, begin_barrier.resource_id, begin_barrier.resource_generation},
                request.config.maximum_commands,
                error,
                0U) ||
            !append_command(
                &staged,
                {NativePlatformCommandKind::BeginRenderPass, 0U, 0U, 0U, request.frame->surface.width, request.frame->surface.height},
                request.config.maximum_commands,
                error,
                0U)) {
            return false;
        }

        for (std::size_t index = 1U; index + 1U < request.commands->commands.size(); ++index) {
            const NativeCommandRecord& source = request.commands->commands[index];
            std::uint32_t flags = 0U;
            if ((source.flags & kNativeCommandPartialDamage) != 0U) {
                flags |= kNativePlatformCommandPartialDamage;
            }
            if ((source.flags & kNativeCommandDuplicatedAcrossDamage) != 0U) {
                flags |= kNativePlatformCommandDuplicatedAcrossDamage;
                if (stats != nullptr) {
                    ++stats->duplicated_commands;
                }
            }
            switch (source.kind) {
                case NativeCommandKind::SetScissor:
                    if (source.payload_index >= request.commands->damage_rects.size()) {
                        set_compile_error(error, NativePlatformCompileErrorKind::CommandTopologyViolation, "scissor index is invalid", index);
                        return false;
                    }
                    if (!append_command(
                            &staged,
                            {NativePlatformCommandKind::SetScissor, source.payload_index, source.scissor_index, flags,
                             request.commands->damage_rects[source.payload_index].inline_size,
                             request.commands->damage_rects[source.payload_index].block_size},
                            request.config.maximum_commands,
                            error,
                            index)) {
                        return false;
                    }
                    if (stats != nullptr) {
                        ++stats->scissor_commands;
                    }
                    break;
                case NativeCommandKind::FillRect:
                    if (source.payload_index >= request.frame->fill_rects.size()) {
                        set_compile_error(error, NativePlatformCompileErrorKind::CommandTopologyViolation, "fill payload index is invalid", index);
                        return false;
                    }
                    if (!append_command(
                            &staged,
                            {NativePlatformCommandKind::FillRect, source.payload_index, source.scissor_index, flags,
                             request.frame->fill_rects[source.payload_index].style_id, 0U},
                            request.config.maximum_commands,
                            error,
                            index)) {
                        return false;
                    }
                    if (stats != nullptr) {
                        ++stats->fill_commands;
                    }
                    break;
                case NativeCommandKind::GlyphBatch: {
                    if (source.payload_index >= request.frame->glyph_batches.size()) {
                        set_compile_error(error, NativePlatformCompileErrorKind::CommandTopologyViolation, "glyph payload index is invalid", index);
                        return false;
                    }
                    const GpuFrameGlyphBatch& batch = request.frame->glyph_batches[source.payload_index];
                    if (batch.first_instance > request.draw_instances.size() ||
                        batch.instance_count > request.draw_instances.size() - batch.first_instance) {
                        set_compile_error(error, NativePlatformCompileErrorKind::DrawTopologyViolation, "glyph batch draw span is invalid", index, batch.first_instance, batch.page_index);
                        return false;
                    }
                    if (batch.page_reference_index >= request.frame->page_references.size()) {
                        set_compile_error(error, NativePlatformCompileErrorKind::CommandTopologyViolation, "glyph page reference is invalid", index, 0U, batch.page_index);
                        return false;
                    }
                    const GpuFramePageReference& page = request.frame->page_references[batch.page_reference_index];
                    if (page.page_index != batch.page_index || page.page_generation != batch.page_generation) {
                        set_compile_error(error, NativePlatformCompileErrorKind::CommandTopologyViolation, "glyph page identity mismatch", index, 0U, batch.page_index);
                        return false;
                    }
                    if (page.required_upload_fence > request.wait_fence_value) {
                        set_compile_error(error, NativePlatformCompileErrorKind::UploadFenceNotReady, "glyph page upload fence is not ready", index, 0U, batch.page_index);
                        return false;
                    }
                    std::uint32_t descriptor_index = 0U;
                    if (!find_or_append_descriptor(
                            &staged,
                            *request.frame,
                            batch,
                            request.config.maximum_descriptors,
                            &descriptor_index,
                            error,
                            index)) {
                        return false;
                    }
                    if (page.required_upload_fence != 0U) {
                        flags |= kNativePlatformCommandWaitsForUpload;
                        if (stats != nullptr) {
                            ++stats->waited_pages;
                        }
                    }
                    if (!append_command(
                            &staged,
                            {NativePlatformCommandKind::BindGlyphTexture, source.payload_index, descriptor_index, flags,
                             page.page_index, page.page_generation},
                            request.config.maximum_commands,
                            error,
                            index) ||
                        !append_command(
                            &staged,
                            {NativePlatformCommandKind::DrawGlyphBatch, source.payload_index, source.scissor_index, flags,
                             batch.first_instance, batch.instance_count},
                            request.config.maximum_commands,
                            error,
                            index)) {
                        return false;
                    }
                    if (stats != nullptr) {
                        ++stats->glyph_draw_commands;
                        stats->maximum_instances_per_draw = std::max<std::uint64_t>(
                            stats->maximum_instances_per_draw,
                            batch.instance_count);
                    }
                    break;
                }
                case NativeCommandKind::BeginRenderPass:
                case NativeCommandKind::EndRenderPass:
                    set_compile_error(error, NativePlatformCompileErrorKind::CommandTopologyViolation, "nested render-pass boundary", index);
                    return false;
            }
        }

        if (!append_command(
                &staged,
                {NativePlatformCommandKind::EndRenderPass, 0U, 0U, 0U, 0U, 0U},
                request.config.maximum_commands,
                error,
                request.commands->commands.size() - 1U)) {
            return false;
        }
        NativePlatformBarrierRecord end_barrier;
        end_barrier.resource_id = request.image.native_resource_id;
        end_barrier.resource_generation = request.image.image.image_generation;
        end_barrier.before = NativePlatformResourceState::RenderTarget;
        end_barrier.after = NativePlatformResourceState::Present;
        end_barrier.source_command_index = static_cast<std::uint32_t>(request.commands->commands.size() - 1U);
        end_barrier.flags = platform_transition_flags(request.config.api_kind);
        if (!append_barrier(
                &staged,
                end_barrier,
                request.config.maximum_barriers,
                error,
                request.commands->commands.size() - 1U) ||
            !append_command(
                &staged,
                {NativePlatformCommandKind::Transition, end_barrier.source_command_index, 1U, end_barrier.flags,
                 end_barrier.resource_id, end_barrier.resource_generation},
                request.config.maximum_commands,
                error,
                request.commands->commands.size() - 1U) ||
            !append_command(
                &staged,
                {NativePlatformCommandKind::EndCommandBuffer, 0U, 0U, 0U, 0U, 0U},
                request.config.maximum_commands,
                error,
                request.commands->commands.size() - 1U) ||
            !append_command(
                &staged,
                {NativePlatformCommandKind::Submit, 0U, 0U, 0U, request.wait_fence_value, request.ticket_id},
                request.config.maximum_commands,
                error,
                request.commands->commands.size() - 1U) ||
            !append_command(
                &staged,
                {NativePlatformCommandKind::Present, 0U, 0U,
                 present_flags(request.config.api_kind, NativePresentMode::Fifo, request.config),
                 request.image.image.image_index, request.ticket_id},
                request.config.maximum_commands,
                error,
                request.commands->commands.size() - 1U)) {
            return false;
        }

        staged.encoded_checksum = submission_checksum(staged);
        output->api_kind = staged.api_kind;
        output->surface = staged.surface;
        output->image = staged.image;
        output->frame_id = staged.frame_id;
        output->ticket_id = staged.ticket_id;
        output->wait_fence_value = staged.wait_fence_value;
        output->command_generation = staged.command_generation;
        output->source_command_checksum = staged.source_command_checksum;
        output->encoded_checksum = staged.encoded_checksum;
        output->commands = std::move(staged.commands);
        output->barriers = std::move(staged.barriers);
        output->descriptors = std::move(staged.descriptors);
        if (stats != nullptr) {
            stats->output_commands = output->commands.size();
            stats->output_barriers = output->barriers.size();
            stats->output_descriptors = output->descriptors.size();
        }
        return true;
    } catch (const std::bad_alloc&) {
        output->release();
        set_compile_error(error, NativePlatformCompileErrorKind::OutputBudgetExceeded, "native platform output budget exhausted");
        return false;
    } catch (...) {
        output->release();
        set_compile_error(error, NativePlatformCompileErrorKind::AggregateOverflow, "unexpected native platform compile failure");
        return false;
    }
}

NativePlatformCapabilities default_native_platform_capabilities(
    NativeGpuApiKind kind) noexcept {
    NativePlatformCapabilities result{};
    if (kind == NativeGpuApiKind::Metal) {
        return result;
    }
    result.maximum_commands = 4096U;
    result.maximum_barriers = 512U;
    result.maximum_descriptors = 512U;
    result.maximum_swapchain_images = 8U;
    result.maximum_frames_in_flight = 8U;
    result.maximum_staging_bytes = 16U * 1024U * 1024U;
    switch (kind) {
        case NativeGpuApiKind::Vulkan:
            result.flags = kNativePlatformTimelineFence |
                kNativePlatformPartialPresent |
                kNativePlatformMailboxPresent |
                kNativePlatformImmediatePresent |
                kNativePlatformTearing |
                kNativePlatformExplicitBarriers;
            break;
        case NativeGpuApiKind::Direct3D12:
            result.flags = kNativePlatformTimelineFence |
                kNativePlatformPartialPresent |
                kNativePlatformMailboxPresent |
                kNativePlatformImmediatePresent |
                kNativePlatformTearing |
                kNativePlatformExplicitBarriers;
            break;
        case NativeGpuApiKind::ReferenceCpu:
            result.flags = kNativePlatformTimelineFence |
                kNativePlatformPartialPresent;
            break;
        case NativeGpuApiKind::Metal:
            break;
    }
    return result;
}

ReferenceNativePlatformDriver::ReferenceNativePlatformDriver(
    NativeGpuApiKind kind,
    NativePlatformCapabilities capabilities) noexcept
    : kind_(kind), capabilities_(capabilities) {
    if (capabilities_.maximum_commands == 0U) {
        capabilities_ = default_native_platform_capabilities(kind_);
    }
}

NativeGpuApiKind ReferenceNativePlatformDriver::kind() const noexcept {
    return kind_;
}

NativePlatformCapabilities ReferenceNativePlatformDriver::capabilities() const noexcept {
    return capabilities_;
}

bool ReferenceNativePlatformDriver::configure_swapchain(
    const GpuSurfaceDescriptor& surface,
    std::uint32_t image_count,
    const NativePlatformAdapterConfig& config,
    NativeGpuApiError* error) noexcept {
    if (kind_ == NativeGpuApiKind::Metal) {
        set_api_error(
            error,
            NativeGpuApiErrorKind::InvalidInput,
            "Metal support was removed from Zevryon");
        return false;
    }
    if (!surface_valid(surface) || !api_kind_supported(kind_) || config.api_kind != kind_ ||
        config.device_generation == 0U || config.driver_generation == 0U ||
        image_count == 0U || image_count > capabilities_.maximum_swapchain_images) {
        set_api_error(error, NativeGpuApiErrorKind::SurfaceConfigurationFailed, "invalid native platform swapchain configuration");
        return false;
    }
    surface_ = surface;
    image_count_ = image_count;
    next_image_index_ = 0U;
    device_generation_ = config.device_generation;
    driver_generation_ = config.driver_generation;
    return true;
}

bool ReferenceNativePlatformDriver::acquire_image(
    const GpuSurfaceDescriptor& surface,
    NativePresentMode mode,
    std::uint64_t ticket_id,
    NativePlatformSwapchainImage* image,
    NativeAcquireStatus* status,
    NativeGpuApiError* error) noexcept {
    if (image == nullptr || status == nullptr || ticket_id == 0U ||
        image_count_ == 0U || surface != surface_ || !mode_supported(mode, capabilities_)) {
        set_api_error(error, NativeGpuApiErrorKind::AcquireFailed, "invalid native platform acquire request");
        return false;
    }
    *status = next_acquire_status_;
    next_acquire_status_ = NativeAcquireStatus::Acquired;
    if (*status != NativeAcquireStatus::Acquired) {
        *image = {};
        return true;
    }
    image->image.device_generation = device_generation_;
    image->image.surface_id = surface_.surface_id;
    image->image.surface_generation = surface_.generation_id;
    image->image.image_generation = next_image_generation_++;
    image->image.image_index = next_image_index_;
    image->image.flags = 1U;
    image->driver_generation = driver_generation_;
    image->native_resource_id = next_native_resource_id_++;
    image->state = NativePlatformResourceState::Present;
    next_image_index_ = (next_image_index_ + 1U) % image_count_;
    return true;
}

bool ReferenceNativePlatformDriver::submit_and_present(
    const NativePlatformSubmission& submission,
    std::uint64_t* signal_fence_value,
    std::uint64_t* encoded_checksum,
    NativePresentStatus* status,
    NativeGpuApiError* error) noexcept {
    if (signal_fence_value == nullptr || encoded_checksum == nullptr || status == nullptr ||
        submission.api_kind != kind_ || submission.surface != surface_ ||
        submission.image.driver_generation != driver_generation_ ||
        submission.image.image.device_generation != device_generation_ ||
        submission.commands.empty() || submission.encoded_checksum == 0U) {
        set_api_error(error, NativeGpuApiErrorKind::EncodeFailed, "invalid native platform submission");
        return false;
    }
    *status = next_present_status_;
    next_present_status_ = NativePresentStatus::Presented;
    if (*status == NativePresentStatus::DeviceLost) {
        set_api_error(error, NativeGpuApiErrorKind::DeviceLost, "native platform device lost");
        return false;
    }
    if (*status == NativePresentStatus::OutOfDate) {
        *signal_fence_value = 0U;
        *encoded_checksum = submission.encoded_checksum;
        return true;
    }
    if (next_fence_value_ == std::numeric_limits<std::uint64_t>::max()) {
        set_api_error(error, NativeGpuApiErrorKind::FenceOverflow, "native platform fence overflow");
        return false;
    }
    *signal_fence_value = next_fence_value_++;
    *encoded_checksum = submission.encoded_checksum;
    return true;
}

void ReferenceNativePlatformDriver::set_next_acquire_status(NativeAcquireStatus status) noexcept {
    next_acquire_status_ = status;
}

void ReferenceNativePlatformDriver::set_next_present_status(NativePresentStatus status) noexcept {
    next_present_status_ = status;
}

NativePlatformGpuCommandApi::NativePlatformGpuCommandApi(
    NativePlatformDriver* driver,
    NativePlatformAdapterConfig config) noexcept
    : driver_(driver), config_(config) {
    if (driver_ != nullptr) {
        capabilities_ = driver_->capabilities();
    }
}

NativeGpuApiKind NativePlatformGpuCommandApi::kind() const noexcept {
    return config_.api_kind;
}

bool NativePlatformGpuCommandApi::configure_surface(
    const GpuSurfaceDescriptor& surface,
    std::uint32_t image_count,
    std::uint64_t device_generation,
    NativeGpuApiError* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (driver_ == nullptr || driver_->kind() != config_.api_kind ||
        !surface_valid(surface) || device_generation != config_.device_generation ||
        image_count == 0U || image_count > config_.maximum_swapchain_images ||
        image_count > capabilities_.maximum_swapchain_images ||
        image_count > acquired_images_.size()) {
        set_api_error(error, NativeGpuApiErrorKind::SurfaceConfigurationFailed, "invalid platform adapter surface configuration");
        return false;
    }
    if ((config_.flags & kNativePlatformRequireTimelineFence) != 0U &&
        (capabilities_.flags & kNativePlatformTimelineFence) == 0U) {
        set_api_error(error, NativeGpuApiErrorKind::SurfaceConfigurationFailed, "timeline fences are required but unsupported");
        return false;
    }
    if (!driver_->configure_swapchain(surface, image_count, config_, error)) {
        return false;
    }
    surface_ = surface;
    image_count_ = image_count;
    acquired_image_count_ = 0U;
    for (NativePlatformSwapchainImage& image : acquired_images_) {
        image = {};
    }
    configured_ = true;
    return true;
}

bool NativePlatformGpuCommandApi::acquire_next_image(
    const GpuSurfaceDescriptor& surface,
    NativePresentMode mode,
    std::uint64_t ticket_id,
    NativeSwapchainImageHandle* image,
    NativeAcquireStatus* status,
    NativeGpuApiError* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_ || driver_ == nullptr || image == nullptr || status == nullptr ||
        surface != surface_ || !mode_supported(mode, capabilities_)) {
        set_api_error(error, NativeGpuApiErrorKind::AcquireFailed, "invalid platform adapter acquire request");
        return false;
    }
    NativePlatformSwapchainImage acquired;
    if (!driver_->acquire_image(surface, mode, ticket_id, &acquired, status, error)) {
        return false;
    }
    if (*status == NativeAcquireStatus::Acquired) {
        if (acquired.image.image_index >= acquired_images_.size()) {
            set_api_error(error, NativeGpuApiErrorKind::AcquireFailed, "platform driver returned an out-of-range image index");
            return false;
        }
        acquired_images_[acquired.image.image_index] = acquired;
        acquired_image_count_ = std::max<std::uint32_t>(
            acquired_image_count_,
            acquired.image.image_index + 1U);
    }
    *image = acquired.image;
    return true;
}

bool NativePlatformGpuCommandApi::encode_submit_present(
    const NativeSwapchainImageHandle& image,
    const NativeCommandBuffer& commands,
    const GpuFrameSubmission& frame,
    std::span<const GlyphAtlasDrawInstance> draw_instances,
    std::uint64_t ticket_id,
    std::uint64_t wait_fence_value,
    std::uint64_t* signal_fence_value,
    std::uint64_t* encoded_checksum,
    NativePresentStatus* status,
    NativeGpuApiError* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_ || driver_ == nullptr || signal_fence_value == nullptr ||
        encoded_checksum == nullptr || status == nullptr || image.surface_id != surface_.surface_id ||
        image.surface_generation != surface_.generation_id ||
        image.device_generation != config_.device_generation) {
        set_api_error(error, NativeGpuApiErrorKind::EncodeFailed, "invalid platform adapter encode request");
        return false;
    }
    try {
        std::array<std::byte, kAdapterScratchBytes> scratch{};
        std::pmr::monotonic_buffer_resource arena(
            scratch.data(),
            scratch.size(),
            std::pmr::null_memory_resource());
        NativePlatformSubmission submission(&arena);
        if (image.image_index >= acquired_image_count_ ||
            image.image_index >= acquired_images_.size() ||
            acquired_images_[image.image_index].image != image) {
            set_api_error(error, NativeGpuApiErrorKind::EncodeFailed, "platform adapter image was not acquired or is stale");
            return false;
        }
        const NativePlatformSwapchainImage platform_image = acquired_images_[image.image_index];
        NativePlatformCompileRequest request;
        request.commands = &commands;
        request.frame = &frame;
        request.draw_instances = draw_instances;
        request.image = platform_image;
        request.ticket_id = ticket_id;
        request.wait_fence_value = wait_fence_value;
        request.config = config_;
        NativePlatformCompileError compile_error;
        if (!compile_native_platform_submission(request, &submission, nullptr, &compile_error)) {
            set_api_error(error, NativeGpuApiErrorKind::EncodeFailed, compile_error.message.c_str());
            return false;
        }
        return driver_->submit_and_present(
            submission,
            signal_fence_value,
            encoded_checksum,
            status,
            error);
    } catch (const std::bad_alloc&) {
        set_api_error(error, NativeGpuApiErrorKind::EncodeFailed, "platform adapter scratch budget exhausted");
        return false;
    } catch (...) {
        set_api_error(error, NativeGpuApiErrorKind::EncodeFailed, "unexpected platform adapter failure");
        return false;
    }
}

NativePlatformAdapterConfig NativePlatformGpuCommandApi::config() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

NativePlatformCapabilities NativePlatformGpuCommandApi::capabilities() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return capabilities_;
}

VulkanNativeGpuCommandApi::VulkanNativeGpuCommandApi(
    NativePlatformDriver* driver,
    NativePlatformAdapterConfig config) noexcept
    : NativePlatformGpuCommandApi(driver, [&config]() {
        config.api_kind = NativeGpuApiKind::Vulkan;
        return config;
    }()) {}

MetalNativeGpuCommandApi::MetalNativeGpuCommandApi(
    NativePlatformDriver* driver,
    NativePlatformAdapterConfig config) noexcept
    : NativePlatformGpuCommandApi(driver, [&config]() {
        config.api_kind = NativeGpuApiKind::Metal;
        return config;
    }()) {}

Direct3D12NativeGpuCommandApi::Direct3D12NativeGpuCommandApi(
    NativePlatformDriver* driver,
    NativePlatformAdapterConfig config) noexcept
    : NativePlatformGpuCommandApi(driver, [&config]() {
        config.api_kind = NativeGpuApiKind::Direct3D12;
        return config;
    }()) {}

} // namespace zevryon::text
