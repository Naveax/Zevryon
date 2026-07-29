#include "native_shader_execution.hpp"

#if defined(ZEVRYON_HAS_METAL_NATIVE_SHADER)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "native_metal_window_context.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace zevryon::text {
namespace {

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
    std::int64_t code = 0) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->native_code = code;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

class MetalNativeShaderExecutionApi final : public NativeShaderExecutionApi {
public:
    ~MetalNativeShaderExecutionApi() override { shutdown(); }

    NativeGpuApiKind kind() const noexcept override {
        return NativeGpuApiKind::Metal;
    }

    NativeShaderCapabilities capabilities() const noexcept override {
        return default_native_shader_capabilities(NativeGpuApiKind::Metal);
    }

    bool configure(
        const NativeGpuSdkContextHandle& context,
        const NativeShaderExecutionLimits& limits,
        NativeShaderExecutionError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        shutdown_locked();
        detail::MetalWindowSharedContext* retained =
            detail::retain_metal_window_context(context);
        if (retained == nullptr || retained->device == nil || retained->queue == nil) {
            if (retained != nullptr) {
                detail::release_metal_window_context(retained);
            }
            return fail(error, NativeShaderExecutionErrorKind::StaleGeneration,
                        "Metal shader execution context is unavailable or stale");
        }
        if (limits.maximum_commands == 0U ||
            limits.maximum_scissors == 0U ||
            limits.maximum_fill_instances == 0U ||
            limits.maximum_glyph_instances == 0U ||
            limits.maximum_atlas_pages == 0U ||
            limits.maximum_atlas_pages > atlas_generations_.size() ||
            limits.maximum_frames_in_flight == 0U ||
            limits.maximum_surface_width == 0U ||
            limits.maximum_surface_height == 0U ||
            limits.maximum_output_bytes == 0U) {
            detail::release_metal_window_context(retained);
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "invalid Metal shader execution limits");
        }

        @autoreleasepool {
            NSError* native_error = nil;
            NSString* source = [[NSString alloc]
                initWithBytes:native_shader_msl_source().data()
                       length:native_shader_msl_source().size()
                     encoding:NSUTF8StringEncoding];
            id<MTLLibrary> library = [retained->device
                newLibraryWithSource:source options:nil error:&native_error];
            if (library == nil) {
                const std::int64_t code = native_error == nil ? 0 : native_error.code;
                detail::release_metal_window_context(retained);
                return fail(error, NativeShaderExecutionErrorKind::ShaderCompilationFailed,
                            "Metal compute shader compilation failed", code);
            }
            id<MTLFunction> function = [library newFunctionWithName:@"zevryon_shader_main"];
            if (function == nil) {
                detail::release_metal_window_context(retained);
                return fail(error, NativeShaderExecutionErrorKind::ShaderCompilationFailed,
                            "Metal compute entry point was not found");
            }
            id<MTLComputePipelineState> pipeline = [retained->device
                newComputePipelineStateWithFunction:function error:&native_error];
            if (pipeline == nil) {
                const std::int64_t code = native_error == nil ? 0 : native_error.code;
                detail::release_metal_window_context(retained);
                return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                            "Metal compute pipeline creation failed", code);
            }
            context_ = retained;
            library_ = library;
            function_ = function;
            pipeline_ = pipeline;
        }

        limits_ = limits;
        snapshot_ = {};
        snapshot_.capabilities = capabilities();
        snapshot_.limits = limits;
        snapshot_.context = context;
        snapshot_.configurations = 1U;
        configured_ = true;
        next_fence_value_ = 1U;
        atlas_generations_.fill(0U);
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
                        "invalid Metal shader execution request");
        }
        if (snapshot_.in_flight_count >= limits_.maximum_frames_in_flight) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "Metal shader frames-in-flight limit exceeded");
        }

        NativeShaderDispatchPlan plan;
        if (!compile_native_shader_dispatch_plan(
                NativeGpuApiKind::Metal, *request.packet, *request.atlas,
                limits_, &plan, error)) {
            return false;
        }
        std::lock_guard<std::mutex> device_lock(context_->device_mutex);
        if (context_->device_generation != snapshot_.context.device_generation ||
            context_->runtime_generation != snapshot_.context.runtime_generation ||
            context_->device == nil || context_->queue == nil) {
            snapshot_.stale_rejections += 1U;
            return fail(error, NativeShaderExecutionErrorKind::StaleGeneration,
                        "Metal shader context generation changed");
        }

        @autoreleasepool {
            if (!ensure_atlas_locked(plan, *request.atlas, error) ||
                !ensure_buffers_locked(plan, error)) {
                return false;
            }
            copy_buffer(commands_, plan.commands.data(), plan.header.command_bytes);
            copy_buffer(fills_, plan.fills.data(), plan.header.fill_bytes);
            copy_buffer(glyphs_, plan.glyphs.data(), plan.header.glyph_bytes);
            copy_buffer(scissors_, plan.scissors.data(), plan.header.scissor_bytes);
            std::memset(output_.contents, 0, static_cast<std::size_t>(plan.header.output_bytes));

            id<MTLCommandBuffer> command_buffer = [context_->queue commandBuffer];
            if (command_buffer == nil) {
                return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                            "Metal command buffer creation failed");
            }
            id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
            if (encoder == nil) {
                return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                            "Metal compute command encoder creation failed");
            }
            [encoder setComputePipelineState:pipeline_];
            [encoder setBytes:&plan.constants length:sizeof(plan.constants) atIndex:0U];
            [encoder setBuffer:commands_ offset:0U atIndex:1U];
            [encoder setBuffer:fills_ offset:0U atIndex:2U];
            [encoder setBuffer:glyphs_ offset:0U atIndex:3U];
            [encoder setBuffer:scissors_ offset:0U atIndex:4U];
            [encoder setBuffer:output_ offset:0U atIndex:5U];
            [encoder setTexture:atlas_texture_ atIndex:0U];
            const MTLSize threads = MTLSizeMake(
                plan.bindings.threadgroup_width,
                plan.bindings.threadgroup_height,
                1U);
            const MTLSize groups = MTLSizeMake(
                plan.header.dispatch_x,
                plan.header.dispatch_y,
                1U);
            [encoder dispatchThreadgroups:groups threadsPerThreadgroup:threads];
            [encoder endEncoding];
            [command_buffer commit];
            [command_buffer waitUntilCompleted];
            if (command_buffer.status == MTLCommandBufferStatusError) {
                const NSError* native_error = command_buffer.error;
                snapshot_.device_lost_events += 1U;
                return fail(error, NativeShaderExecutionErrorKind::DeviceLost,
                            "Metal compute command buffer completed with an error",
                            native_error == nil ? 0 : native_error.code);
            }
        }

        const std::span<const std::byte> output_bytes(
            static_cast<const std::byte*>(output_.contents),
            static_cast<std::size_t>(plan.header.output_bytes));
        const std::uint64_t readback_checksum = shader_bytes_checksum(output_bytes);
        if ((request.flags & kNativeShaderExecutionRequireExactReadback) != 0U &&
            request.expected_readback_checksum != 0U &&
            readback_checksum != request.expected_readback_checksum) {
            return fail(error, NativeShaderExecutionErrorKind::ReadbackMismatch,
                        "Metal GPU readback differs from reference checksum");
        }

        if (readback != nullptr) {
            try {
                ShaderReadback candidate;
                candidate.width = request.packet->header.surface_width;
                candidate.height = request.packet->header.surface_height;
                candidate.row_bytes = candidate.width * 4U;
                candidate.checksum = readback_checksum;
                candidate.bgra.assign(output_bytes.begin(), output_bytes.end());
                *readback = std::move(candidate);
            } catch (const std::bad_alloc&) {
                return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                            "Metal shader readback allocation failed");
            }
        }

        const std::uint64_t signal = next_fence_value_++;
        *receipt = {};
        receipt->api_kind = NativeGpuApiKind::Metal;
        receipt->flags = request.flags;
        receipt->command_count = request.packet->header.command_count;
        receipt->fill_instance_count = request.packet->header.fill_instance_count;
        receipt->glyph_instance_count = request.packet->header.glyph_instance_count;
        receipt->atlas_binding_count = plan.header.atlas_binding_count;
        receipt->dispatch_x = plan.header.dispatch_x;
        receipt->dispatch_y = plan.header.dispatch_y;
        receipt->frame_id = request.packet->header.frame_id;
        receipt->ticket_id = request.ticket_id;
        receipt->wait_fence_value = request.wait_fence_value;
        receipt->signal_fence_value = signal;
        receipt->packet_checksum = request.packet->header.packet_checksum;
        receipt->plan_checksum = plan.header.plan_checksum;
        receipt->readback_checksum = readback_checksum;
        receipt->output_bytes = plan.header.output_bytes;

        snapshot_.executions += 1U;
        snapshot_.readbacks += 1U;
        snapshot_.last_submitted_fence_value = signal;
        snapshot_.completed_fence_value = signal;
        snapshot_.resident_atlas_pages = plan.header.atlas_binding_count;
        snapshot_.current_device_bytes = atlas_bytes_ + plan.header.output_bytes;
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
        if (!configured_ ||
            completed_fence_value < snapshot_.completed_fence_value ||
            completed_fence_value > snapshot_.last_submitted_fence_value) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "Metal shader completion fence is outside timeline");
        }
        snapshot_.completed_fence_value = completed_fence_value;
        snapshot_.in_flight_count = 0U;
        snapshot_.current_staging_bytes = 0U;
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
    static void copy_buffer(id<MTLBuffer> buffer, const void* source,
                            std::uint64_t bytes) noexcept {
        if (bytes != 0U) {
            std::memcpy(buffer.contents, source, static_cast<std::size_t>(bytes));
        }
    }

    bool ensure_buffer_locked(
        id<MTLBuffer>* buffer,
        std::uint64_t* capacity,
        std::uint64_t required,
        NativeShaderExecutionError* error) noexcept {
        if (required == 0U) {
            required = 4U;
        }
        if (*buffer != nil && *capacity >= required) {
            return true;
        }
        id<MTLBuffer> replacement = [context_->device
            newBufferWithLength:static_cast<NSUInteger>(required)
                     options:MTLResourceStorageModeShared];
        if (replacement == nil) {
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "Metal shader buffer allocation failed");
        }
        *buffer = replacement;
        *capacity = required;
        return true;
    }

    bool ensure_buffers_locked(
        const NativeShaderDispatchPlan& plan,
        NativeShaderExecutionError* error) noexcept {
        return ensure_buffer_locked(&commands_, &commands_capacity_,
                                    plan.header.command_bytes, error) &&
            ensure_buffer_locked(&fills_, &fills_capacity_,
                                 plan.header.fill_bytes, error) &&
            ensure_buffer_locked(&glyphs_, &glyphs_capacity_,
                                 plan.header.glyph_bytes, error) &&
            ensure_buffer_locked(&scissors_, &scissors_capacity_,
                                 plan.header.scissor_bytes, error) &&
            ensure_buffer_locked(&output_, &output_capacity_,
                                 plan.header.output_bytes, error);
    }

    bool ensure_atlas_locked(
        const NativeShaderDispatchPlan& plan,
        const ShaderAtlasResidency& atlas,
        NativeShaderExecutionError* error) noexcept {
        std::uint32_t width = 1U;
        std::uint32_t height = 1U;
        for (const NativeShaderAtlasBinding& binding : plan.atlas_bindings) {
            width = std::max(width, static_cast<std::uint32_t>(binding.width));
            height = std::max(height, static_cast<std::uint32_t>(binding.height));
        }
        if (atlas_texture_ == nil || atlas_width_ < width || atlas_height_ < height ||
            atlas_layers_ < limits_.maximum_atlas_pages) {
            MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
            descriptor.textureType = MTLTextureType2DArray;
            descriptor.pixelFormat = MTLPixelFormatRGBA8Uint;
            descriptor.width = width;
            descriptor.height = height;
            descriptor.arrayLength = limits_.maximum_atlas_pages;
            descriptor.mipmapLevelCount = 1U;
            descriptor.storageMode = MTLStorageModeManaged;
            descriptor.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> replacement =
                [context_->device newTextureWithDescriptor:descriptor];
            if (replacement == nil) {
                return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                            "Metal persistent atlas texture allocation failed");
            }
            atlas_texture_ = replacement;
            atlas_width_ = width;
            atlas_height_ = height;
            atlas_layers_ = limits_.maximum_atlas_pages;
            atlas_generations_.fill(0U);
            atlas_bytes_ = static_cast<std::uint64_t>(width) * height *
                atlas_layers_ * 4U;
        }
        for (const NativeShaderAtlasBinding& binding : plan.atlas_bindings) {
            if (binding.texture_layer >= atlas_generations_.size()) {
                return fail(error, NativeShaderExecutionErrorKind::InvalidAtlasReference,
                            "Metal atlas texture layer is outside the configured array");
            }
            if (atlas_generations_[binding.texture_layer] ==
                binding.page_generation) {
                continue;
            }
            const ShaderAtlasResidentPage* page = atlas.find(
                binding.page_index, binding.page_generation);
            if (page == nullptr || page->canonical_bgra.size() != binding.resident_bytes) {
                return fail(error, NativeShaderExecutionErrorKind::InvalidAtlasReference,
                            "Metal atlas binding is no longer resident");
            }
            const MTLRegion region = MTLRegionMake2D(
                0U, 0U, binding.width, binding.height);
            [atlas_texture_ replaceRegion:region
                             mipmapLevel:0U
                                   slice:binding.texture_layer
                               withBytes:page->canonical_bgra.data()
                             bytesPerRow:binding.row_bytes
                           bytesPerImage:static_cast<NSUInteger>(binding.resident_bytes)];
            atlas_generations_[binding.texture_layer] = binding.page_generation;
            snapshot_.atlas_uploads += 1U;
        }
        return true;
    }

    void shutdown_locked() noexcept {
        commands_ = nil;
        fills_ = nil;
        glyphs_ = nil;
        scissors_ = nil;
        output_ = nil;
        atlas_texture_ = nil;
        pipeline_ = nil;
        function_ = nil;
        library_ = nil;
        commands_capacity_ = 0U;
        fills_capacity_ = 0U;
        glyphs_capacity_ = 0U;
        scissors_capacity_ = 0U;
        output_capacity_ = 0U;
        atlas_width_ = 0U;
        atlas_height_ = 0U;
        atlas_layers_ = 0U;
        atlas_bytes_ = 0U;
        atlas_generations_.fill(0U);
        if (context_ != nullptr) {
            detail::release_metal_window_context(context_);
            context_ = nullptr;
        }
        snapshot_ = {};
        limits_ = {};
        configured_ = false;
        next_fence_value_ = 1U;
    }

    mutable std::mutex mutex_;
    detail::MetalWindowSharedContext* context_{nullptr};
    NativeShaderExecutionLimits limits_;
    NativeShaderExecutionSnapshot snapshot_;
    id<MTLLibrary> library_{nil};
    id<MTLFunction> function_{nil};
    id<MTLComputePipelineState> pipeline_{nil};
    id<MTLBuffer> commands_{nil};
    id<MTLBuffer> fills_{nil};
    id<MTLBuffer> glyphs_{nil};
    id<MTLBuffer> scissors_{nil};
    id<MTLBuffer> output_{nil};
    id<MTLTexture> atlas_texture_{nil};
    std::uint64_t commands_capacity_{0U};
    std::uint64_t fills_capacity_{0U};
    std::uint64_t glyphs_capacity_{0U};
    std::uint64_t scissors_capacity_{0U};
    std::uint64_t output_capacity_{0U};
    std::uint32_t atlas_width_{0U};
    std::uint32_t atlas_height_{0U};
    std::uint32_t atlas_layers_{0U};
    std::uint64_t atlas_bytes_{0U};
    std::array<std::uint32_t, 16U> atlas_generations_{};
    std::uint64_t next_fence_value_{1U};
    bool configured_{false};
};

} // namespace

std::unique_ptr<NativeShaderExecutionApi>
make_metal_native_shader_execution_api() noexcept {
    try {
        return std::make_unique<MetalNativeShaderExecutionApi>();
    } catch (...) {
        return nullptr;
    }
}

} // namespace zevryon::text

#endif // ZEVRYON_HAS_METAL_NATIVE_SHADER
