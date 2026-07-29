#include "native_shader_execution.hpp"

#if defined(ZEVRYON_HAS_METAL_SHADER_EXECUTION)

#include "native_metal_window_context.hpp"
#include "native_shader_execution_metal_metallib.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <dispatch/dispatch.h>

#include <algorithm>
#include <array>
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

using detail::MetalWindowSharedContext;

struct MetalFill final {
    std::int32_t x{0};
    std::int32_t y{0};
    std::int32_t width{0};
    std::int32_t height{0};
    std::uint32_t color{0U};
    std::uint32_t reserved[3]{0U, 0U, 0U};
};
static_assert(sizeof(MetalFill) == 32U);

struct MetalGlyph final {
    std::int32_t x{0};
    std::int32_t y{0};
    std::int32_t width{0};
    std::int32_t height{0};
    std::uint32_t atlas_slice{0U};
    std::uint32_t atlas_x{0U};
    std::uint32_t atlas_y{0U};
    std::uint32_t atlas_width{0U};
    std::uint32_t atlas_height{0U};
    std::uint32_t color{0U};
    std::uint32_t format{0U};
    std::uint32_t reserved[5]{0U, 0U, 0U, 0U, 0U};
};
static_assert(sizeof(MetalGlyph) == 64U);

struct MetalDispatchConstants final {
    std::uint32_t surface_width{0U};
    std::uint32_t surface_height{0U};
    std::uint32_t operation{0U};
    std::uint32_t instance_index{0U};
    std::uint32_t dispatch_origin_x{0U};
    std::uint32_t dispatch_origin_y{0U};
    std::uint32_t dispatch_width{0U};
    std::uint32_t dispatch_height{0U};
};
static_assert(sizeof(MetalDispatchConstants) == 32U);

struct AtlasPageView final {
    const ShaderAtlasResidentPage* page{nullptr};
};

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
    std::int64_t native_code = 0) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->native_code = native_code;
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

std::uint32_t pack_color(ShaderColorBgra8 color) noexcept {
    return static_cast<std::uint32_t>(color.blue) |
        (static_cast<std::uint32_t>(color.green) << 8U) |
        (static_cast<std::uint32_t>(color.red) << 16U) |
        (static_cast<std::uint32_t>(color.alpha) << 24U);
}

bool intersect_dispatch_rect(
    const ShaderRectI& destination,
    const ShaderRectI& scissor,
    std::uint32_t surface_width,
    std::uint32_t surface_height,
    MetalDispatchConstants* constants) noexcept {
    if (constants == nullptr || destination.width <= 0 || destination.height <= 0 ||
        scissor.width <= 0 || scissor.height <= 0) {
        return false;
    }
    const std::int64_t left = std::max<std::int64_t>(
        {destination.x, scissor.x, 0});
    const std::int64_t top = std::max<std::int64_t>(
        {destination.y, scissor.y, 0});
    const std::int64_t right = std::min<std::int64_t>(
        {static_cast<std::int64_t>(destination.x) + destination.width,
         static_cast<std::int64_t>(scissor.x) + scissor.width,
         surface_width});
    const std::int64_t bottom = std::min<std::int64_t>(
        {static_cast<std::int64_t>(destination.y) + destination.height,
         static_cast<std::int64_t>(scissor.y) + scissor.height,
         surface_height});
    if (right <= left || bottom <= top) {
        return false;
    }
    constants->dispatch_origin_x = static_cast<std::uint32_t>(left);
    constants->dispatch_origin_y = static_cast<std::uint32_t>(top);
    constants->dispatch_width = static_cast<std::uint32_t>(right - left);
    constants->dispatch_height = static_cast<std::uint32_t>(bottom - top);
    return true;
}

class MetalShaderExecutor final : public NativeShaderExecutor {
public:
    ~MetalShaderExecutor() override { shutdown(); }

    NativeGpuApiKind kind() const noexcept override {
        return NativeGpuApiKind::Metal;
    }

    bool configure(
        const NativeShaderExecutionConfig& config,
        NativeShaderExecutionError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (config.context.api_kind != NativeGpuApiKind::Metal ||
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
                        "invalid Metal shader executor configuration");
        }
        reset_locked();
        MetalWindowSharedContext* retained =
            detail::retain_metal_window_context(config.context);
        if (retained == nullptr) {
            return fail(error, NativeShaderExecutionErrorKind::NativeContextUnavailable,
                        "retained Metal context is unavailable");
        }

        bool configured = false;
        @autoreleasepool {
            std::lock_guard<std::mutex> device_lock(retained->device_mutex);
            if (retained->device != nil && retained->queue != nil) {
                dispatch_data_t data = dispatch_data_create(
                    detail::kMetalIntegerComposerMetallib.data(),
                    detail::kMetalIntegerComposerMetallib.size(),
                    dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                    DISPATCH_DATA_DESTRUCTOR_NONE);
                NSError* native_error = nil;
                id<MTLLibrary> library = [retained->device
                    newLibraryWithData:data error:&native_error];
                id<MTLFunction> function = library == nil
                    ? nil : [library newFunctionWithName:@"zevryon_integer_composer"];
                id<MTLComputePipelineState> pipeline = function == nil
                    ? nil : [retained->device
                        newComputePipelineStateWithFunction:function
                        error:&native_error];
                if (pipeline != nil) {
                    context_ = retained;
                    library_ = library;
                    function_ = function;
                    pipeline_ = pipeline;
                    configured = true;
                } else {
                    const std::int64_t code = native_error == nil
                        ? 0 : static_cast<std::int64_t>(native_error.code);
                    fail(error,
                         function == nil
                             ? NativeShaderExecutionErrorKind::ShaderCompilationFailed
                             : NativeShaderExecutionErrorKind::PipelineCreationFailed,
                         function == nil
                             ? "Metal integer compute entry point is unavailable"
                             : "Metal integer compute pipeline creation failed",
                         code);
                }
            }
        }
        if (!configured) {
            detail::release_metal_window_context(retained);
            return false;
        }

        config_ = config;
        snapshot_ = {};
        snapshot_.api_kind = NativeGpuApiKind::Metal;
        snapshot_.configured = 1U;
        snapshot_.capability_flags =
            kNativeShaderExecutionIntegerComposition |
            kNativeShaderExecutionPersistentAtlas |
            kNativeShaderExecutionGpuReadback |
            kNativeShaderExecutionRetainedContext;
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
            readback == nullptr || packet.header.frame_id == 0U ||
            packet.header.packet_checksum != shader_packet_checksum(packet) ||
            packet.header.command_count != packet.commands.size() ||
            packet.header.fill_instance_count != packet.fills.size() ||
            packet.header.glyph_instance_count != packet.glyphs.size() ||
            packet.header.scissor_count != packet.scissors.size() ||
            packet.header.surface_width == 0U ||
            packet.header.surface_height == 0U) {
            snapshot_.rejected_packets += 1U;
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "invalid or mutated Metal shader packet");
        }
        if (packet.commands.size() > config_.limits.maximum_commands ||
            packet.fills.size() > config_.limits.maximum_fill_instances ||
            packet.glyphs.size() > config_.limits.maximum_glyph_instances ||
            packet.header.packet_bytes > config_.limits.maximum_packet_bytes ||
            packet.header.surface_width > config_.limits.maximum_surface_width ||
            packet.header.surface_height > config_.limits.maximum_surface_height ||
            atlas.resident_bytes() > config_.limits.maximum_atlas_bytes) {
            snapshot_.rejected_packets += 1U;
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "shader packet exceeds Metal execution limits");
        }

        try {
            std::lock_guard<std::mutex> device_lock(context_->device_mutex);
            if (context_->device == nil || context_->queue == nil ||
                context_->device_generation != config_.context.device_generation ||
                context_->runtime_generation != config_.context.runtime_generation) {
                snapshot_.rejected_packets += 1U;
                return fail(error, NativeShaderExecutionErrorKind::StaleGeneration,
                            "Metal shader executor context generation is stale");
            }

            std::vector<AtlasPageView> pages;
            std::vector<MetalFill> fills;
            std::vector<MetalGlyph> glyphs;
            if (!collect_pages_locked(packet, atlas, &pages, error) ||
                !build_records_locked(packet, &fills, &glyphs, error) ||
                !ensure_atlas_locked(pages, error) ||
                !ensure_buffers_locked(packet, fills, glyphs, error) ||
                !record_and_wait_locked(packet, error) ||
                !publish_readback_locked(packet, readback, error)) {
                return false;
            }

            snapshot_.executions += 1U;
            snapshot_.readbacks += 1U;
            snapshot_.last_packet_checksum = packet.header.packet_checksum;
            snapshot_.last_readback_checksum = readback->checksum;
            return true;
        } catch (const std::bad_alloc&) {
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "Metal shader execution allocation failed");
        } catch (...) {
            return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                        "unexpected Metal shader execution failure");
        }
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
    bool collect_pages_locked(
        const GpuShaderPacket& packet,
        const ShaderAtlasResidency& atlas,
        std::vector<AtlasPageView>* pages,
        NativeShaderExecutionError* error) noexcept {
        if (pages == nullptr) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "Metal atlas page output is null");
        }
        pages->clear();
        try {
            pages->reserve(packet.glyphs.size());
            for (const GpuShaderGlyphInstance& glyph : packet.glyphs) {
                const ShaderAtlasResidentPage* page = atlas.find(
                    glyph.atlas_page_index, glyph.atlas_page_generation);
                if (page == nullptr || page->canonical_bgra.empty() ||
                    page->page_index >= config_.limits.maximum_atlas_pages) {
                    return fail(error, NativeShaderExecutionErrorKind::AtlasUploadFailed,
                                "Metal glyph references a missing atlas page");
                }
                const auto duplicate = std::find_if(
                    pages->begin(), pages->end(),
                    [page](const AtlasPageView& candidate) {
                        return candidate.page->page_index == page->page_index;
                    });
                if (duplicate == pages->end()) {
                    pages->push_back({page});
                } else if (duplicate->page->page_generation != page->page_generation) {
                    return fail(error, NativeShaderExecutionErrorKind::StaleGeneration,
                                "Metal packet references conflicting page generations");
                }
            }
            std::sort(
                pages->begin(), pages->end(),
                [](const AtlasPageView& left, const AtlasPageView& right) {
                    return left.page->page_index < right.page->page_index;
                });
            return true;
        } catch (const std::bad_alloc&) {
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "Metal atlas page collection allocation failed");
        }
    }

    bool build_records_locked(
        const GpuShaderPacket& packet,
        std::vector<MetalFill>* fills,
        std::vector<MetalGlyph>* glyphs,
        NativeShaderExecutionError* error) noexcept {
        if (fills == nullptr || glyphs == nullptr) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "Metal record output is null");
        }
        fills->clear();
        glyphs->clear();
        try {
            fills->reserve(packet.fills.size());
            glyphs->reserve(packet.glyphs.size());
            for (const GpuShaderFillInstance& source : packet.fills) {
                fills->push_back({
                    source.destination.x,
                    source.destination.y,
                    source.destination.width,
                    source.destination.height,
                    pack_color(source.color),
                    {0U, 0U, 0U}});
            }
            for (const GpuShaderGlyphInstance& source : packet.glyphs) {
                MetalGlyph glyph{};
                glyph.x = source.destination.x;
                glyph.y = source.destination.y;
                glyph.width = source.destination.width;
                glyph.height = source.destination.height;
                glyph.atlas_slice = source.atlas_page_index;
                glyph.atlas_x = source.atlas_x;
                glyph.atlas_y = source.atlas_y;
                glyph.atlas_width = source.atlas_width;
                glyph.atlas_height = source.atlas_height;
                glyph.color = pack_color(source.color);
                glyph.format = static_cast<std::uint32_t>(source.format);
                glyphs->push_back(glyph);
            }
            return true;
        } catch (const std::bad_alloc&) {
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "Metal record allocation failed");
        }
    }

    bool ensure_buffer_locked(
        id<MTLBuffer>* buffer,
        std::uint64_t* capacity,
        std::uint64_t required,
        NativeShaderExecutionError* error) noexcept {
        if (buffer == nullptr || capacity == nullptr) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "Metal buffer target is null");
        }
        required = std::max<std::uint64_t>(required, 4U);
        if (required > (std::numeric_limits<NSUInteger>::max)()) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "Metal buffer length exceeds NSUInteger");
        }
        if (*buffer != nil && *capacity >= required) {
            return true;
        }
        id<MTLBuffer> replacement = [context_->device
            newBufferWithLength:static_cast<NSUInteger>(required)
            options:MTLResourceStorageModeShared];
        if (replacement == nil) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                        "Metal shared buffer allocation failed");
        }
        *buffer = replacement;
        *capacity = required;
        return true;
    }

    bool ensure_buffers_locked(
        const GpuShaderPacket& packet,
        const std::vector<MetalFill>& fills,
        const std::vector<MetalGlyph>& glyphs,
        NativeShaderExecutionError* error) noexcept {
        std::uint64_t output_pixels = 0U;
        std::uint64_t output_bytes = 0U;
        if (!checked_multiply(
                packet.header.surface_width,
                packet.header.surface_height,
                &output_pixels) ||
            !checked_multiply(output_pixels, 4U, &output_bytes) ||
            output_bytes > config_.limits.maximum_readback_bytes) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "Metal output surface exceeds readback budget");
        }
        const std::uint64_t fill_bytes =
            static_cast<std::uint64_t>(fills.size()) * sizeof(MetalFill);
        const std::uint64_t glyph_bytes =
            static_cast<std::uint64_t>(glyphs.size()) * sizeof(MetalGlyph);
        if (!ensure_buffer_locked(
                &fill_buffer_, &fill_capacity_, fill_bytes, error) ||
            !ensure_buffer_locked(
                &glyph_buffer_, &glyph_capacity_, glyph_bytes, error) ||
            !ensure_buffer_locked(
                &output_buffer_, &output_capacity_, output_bytes, error)) {
            return false;
        }
        if (fill_bytes != 0U) {
            std::memcpy(
                fill_buffer_.contents, fills.data(),
                static_cast<std::size_t>(fill_bytes));
        }
        if (glyph_bytes != 0U) {
            std::memcpy(
                glyph_buffer_.contents, glyphs.data(),
                static_cast<std::size_t>(glyph_bytes));
        }
        output_width_ = packet.header.surface_width;
        output_height_ = packet.header.surface_height;
        snapshot_.output_surface_bytes = output_bytes;
        snapshot_.peak_transient_bytes = std::max(
            snapshot_.peak_transient_bytes, fill_bytes + glyph_bytes);
        return true;
    }

    bool ensure_atlas_locked(
        const std::vector<AtlasPageView>& pages,
        NativeShaderExecutionError* error) noexcept {
        std::uint32_t required_width = 1U;
        std::uint32_t required_height = 1U;
        std::uint32_t required_layers = 1U;
        std::uint64_t resident_bytes = 0U;
        for (const AtlasPageView& view : pages) {
            required_width = std::max<std::uint32_t>(required_width, view.page->width);
            required_height = std::max<std::uint32_t>(required_height, view.page->height);
            required_layers = std::max<std::uint32_t>(
                required_layers, view.page->page_index + 1U);
            if (view.page->canonical_bgra.size() >
                (std::numeric_limits<std::uint64_t>::max)() - resident_bytes) {
                return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                            "Metal atlas byte count overflowed");
            }
            resident_bytes += view.page->canonical_bgra.size();
        }
        if (resident_bytes > config_.limits.maximum_atlas_bytes ||
            required_layers > config_.limits.maximum_atlas_pages) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "Metal atlas residency exceeds configured limits");
        }

        bool recreated = atlas_texture_ == nil ||
            atlas_width_ < required_width ||
            atlas_height_ < required_height ||
            atlas_layers_ < required_layers;
        if (recreated) {
            MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
            descriptor.textureType = MTLTextureType2DArray;
            descriptor.pixelFormat = MTLPixelFormatR32Uint;
            descriptor.width = required_width;
            descriptor.height = required_height;
            descriptor.arrayLength = required_layers;
            descriptor.mipmapLevelCount = 1U;
            descriptor.sampleCount = 1U;
            descriptor.storageMode = MTLStorageModeManaged;
            descriptor.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> replacement =
                [context_->device newTextureWithDescriptor:descriptor];
            if (replacement == nil) {
                return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                            "Metal atlas texture allocation failed");
            }
            atlas_texture_ = replacement;
            atlas_width_ = required_width;
            atlas_height_ = required_height;
            atlas_layers_ = required_layers;
            atlas_generations_.assign(required_layers, 0U);
            atlas_checksums_.assign(required_layers, 0U);
            atlas_widths_.assign(required_layers, 0U);
            atlas_heights_.assign(required_layers, 0U);
        }

        bool uploaded = false;
        for (const AtlasPageView& view : pages) {
            const ShaderAtlasResidentPage& page = *view.page;
            const std::uint64_t checksum = shader_bytes_checksum(page.canonical_bgra);
            const std::size_t index = page.page_index;
            const bool changed = recreated ||
                atlas_generations_[index] != page.page_generation ||
                atlas_checksums_[index] != checksum ||
                atlas_widths_[index] != page.width ||
                atlas_heights_[index] != page.height;
            if (!changed) {
                continue;
            }
            const std::uint64_t row_bytes =
                static_cast<std::uint64_t>(page.width) * 4U;
            const std::uint64_t image_bytes = row_bytes * page.height;
            if (page.canonical_bgra.size() != image_bytes) {
                return fail(error, NativeShaderExecutionErrorKind::AtlasUploadFailed,
                            "Metal canonical atlas page size is invalid");
            }
            const MTLRegion region = MTLRegionMake2D(0U, 0U, page.width, page.height);
            [atlas_texture_
                replaceRegion:region
                mipmapLevel:0U
                slice:page.page_index
                withBytes:page.canonical_bgra.data()
                bytesPerRow:static_cast<NSUInteger>(row_bytes)
                bytesPerImage:static_cast<NSUInteger>(image_bytes)];
            atlas_generations_[index] = page.page_generation;
            atlas_checksums_[index] = checksum;
            atlas_widths_[index] = page.width;
            atlas_heights_[index] = page.height;
            uploaded = true;
        }
        snapshot_.persistent_atlas_bytes = resident_bytes;
        if (uploaded) {
            snapshot_.atlas_upload_batches += 1U;
        } else {
            snapshot_.atlas_reuses += 1U;
        }
        return true;
    }

    bool dispatch_locked(
        id<MTLComputeCommandEncoder> encoder,
        MetalDispatchConstants constants) noexcept {
        [encoder setBytes:&constants
                   length:sizeof(constants)
                  atIndex:3U];
        const NSUInteger groups_x =
            (static_cast<NSUInteger>(constants.dispatch_width) + 7U) / 8U;
        const NSUInteger groups_y =
            (static_cast<NSUInteger>(constants.dispatch_height) + 7U) / 8U;
        [encoder dispatchThreadgroups:MTLSizeMake(groups_x, groups_y, 1U)
                threadsPerThreadgroup:MTLSizeMake(8U, 8U, 1U)];
        return true;
    }

    bool record_and_wait_locked(
        const GpuShaderPacket& packet,
        NativeShaderExecutionError* error) noexcept {
        @autoreleasepool {
            id<MTLCommandBuffer> command_buffer = [context_->queue commandBuffer];
            if (command_buffer == nil) {
                return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                            "Metal command buffer creation failed");
            }
            id<MTLComputeCommandEncoder> encoder =
                [command_buffer computeCommandEncoder];
            if (encoder == nil) {
                return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                            "Metal compute encoder creation failed");
            }
            [encoder setComputePipelineState:pipeline_];
            [encoder setBuffer:fill_buffer_ offset:0U atIndex:0U];
            [encoder setBuffer:glyph_buffer_ offset:0U atIndex:1U];
            [encoder setBuffer:output_buffer_ offset:0U atIndex:2U];
            [encoder setTexture:atlas_texture_ atIndex:0U];

            MetalDispatchConstants constants{};
            constants.surface_width = packet.header.surface_width;
            constants.surface_height = packet.header.surface_height;
            constants.operation = 0U;
            constants.dispatch_width = packet.header.surface_width;
            constants.dispatch_height = packet.header.surface_height;
            dispatch_locked(encoder, constants);
            [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];

            for (const GpuShaderDrawCommand& command : packet.commands) {
                if (command.scissor_index >= packet.scissors.size()) {
                    [encoder endEncoding];
                    return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                                "Metal command references an invalid scissor");
                }
                const ShaderRectI scissor =
                    packet.scissors[command.scissor_index].rect;
                if (command.kind == ShaderPrimitiveKind::Fill) {
                    if (command.first_instance > packet.fills.size() ||
                        command.instance_count >
                            packet.fills.size() - command.first_instance) {
                        [encoder endEncoding];
                        return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                                    "Metal fill command span is invalid");
                    }
                    for (std::uint32_t offset = 0U;
                         offset < command.instance_count; ++offset) {
                        const std::uint32_t index = command.first_instance + offset;
                        constants = {};
                        constants.surface_width = packet.header.surface_width;
                        constants.surface_height = packet.header.surface_height;
                        constants.operation = 1U;
                        constants.instance_index = index;
                        if (!intersect_dispatch_rect(
                                packet.fills[index].destination, scissor,
                                packet.header.surface_width,
                                packet.header.surface_height,
                                &constants)) {
                            continue;
                        }
                        dispatch_locked(encoder, constants);
                        [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
                    }
                } else if (command.kind == ShaderPrimitiveKind::GlyphBatch) {
                    if (command.first_instance > packet.glyphs.size() ||
                        command.instance_count >
                            packet.glyphs.size() - command.first_instance) {
                        [encoder endEncoding];
                        return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                                    "Metal glyph command span is invalid");
                    }
                    for (std::uint32_t offset = 0U;
                         offset < command.instance_count; ++offset) {
                        const std::uint32_t index = command.first_instance + offset;
                        constants = {};
                        constants.surface_width = packet.header.surface_width;
                        constants.surface_height = packet.header.surface_height;
                        constants.operation = 2U;
                        constants.instance_index = index;
                        if (!intersect_dispatch_rect(
                                packet.glyphs[index].destination, scissor,
                                packet.header.surface_width,
                                packet.header.surface_height,
                                &constants)) {
                            continue;
                        }
                        dispatch_locked(encoder, constants);
                        [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
                    }
                } else {
                    [encoder endEncoding];
                    return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                                "Metal command kind is unsupported");
                }
            }
            [encoder endEncoding];
            [command_buffer commit];
            [command_buffer waitUntilCompleted];
            if (command_buffer.status == MTLCommandBufferStatusError) {
                const NSError* native_error = command_buffer.error;
                return fail(error, NativeShaderExecutionErrorKind::DeviceLost,
                            "Metal integer compute command buffer failed",
                            native_error == nil
                                ? 0 : static_cast<std::int64_t>(native_error.code));
            }
            return true;
        }
    }

    bool publish_readback_locked(
        const GpuShaderPacket& packet,
        ShaderReadback* readback,
        NativeShaderExecutionError* error) noexcept {
        if (output_buffer_ == nil || readback == nullptr) {
            return fail(error, NativeShaderExecutionErrorKind::ReadbackFailed,
                        "Metal output buffer is unavailable");
        }
        const std::size_t bytes = static_cast<std::size_t>(
            snapshot_.output_surface_bytes);
        const auto* begin = static_cast<const std::byte*>(output_buffer_.contents);
        if (begin == nullptr) {
            return fail(error, NativeShaderExecutionErrorKind::ReadbackFailed,
                        "Metal output buffer mapping is unavailable");
        }
        try {
            ShaderReadback candidate;
            candidate.width = packet.header.surface_width;
            candidate.height = packet.header.surface_height;
            candidate.row_bytes = packet.header.surface_width * 4U;
            candidate.bgra.assign(begin, begin + bytes);
            candidate.checksum = shader_bytes_checksum(candidate.bgra);
            *readback = std::move(candidate);
            return true;
        } catch (const std::bad_alloc&) {
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "Metal readback publication allocation failed");
        }
    }

    void reset_locked() noexcept {
        MetalWindowSharedContext* released = context_;
        if (released != nullptr) {
            {
                std::lock_guard<std::mutex> device_lock(released->device_mutex);
                atlas_texture_ = nil;
                fill_buffer_ = nil;
                glyph_buffer_ = nil;
                output_buffer_ = nil;
                pipeline_ = nil;
                function_ = nil;
                library_ = nil;
            }
            context_ = nullptr;
            detail::release_metal_window_context(released);
        } else {
            atlas_texture_ = nil;
            fill_buffer_ = nil;
            glyph_buffer_ = nil;
            output_buffer_ = nil;
            pipeline_ = nil;
            function_ = nil;
            library_ = nil;
        }
        atlas_generations_.clear();
        atlas_checksums_.clear();
        atlas_widths_.clear();
        atlas_heights_.clear();
        atlas_width_ = 0U;
        atlas_height_ = 0U;
        atlas_layers_ = 0U;
        fill_capacity_ = 0U;
        glyph_capacity_ = 0U;
        output_capacity_ = 0U;
        output_width_ = 0U;
        output_height_ = 0U;
        config_ = {};
        snapshot_ = {};
    }

    mutable std::mutex mutex_;
    NativeShaderExecutionConfig config_{};
    NativeShaderExecutionSnapshot snapshot_{};
    MetalWindowSharedContext* context_{nullptr};
    id<MTLLibrary> library_{nil};
    id<MTLFunction> function_{nil};
    id<MTLComputePipelineState> pipeline_{nil};
    id<MTLBuffer> fill_buffer_{nil};
    id<MTLBuffer> glyph_buffer_{nil};
    id<MTLBuffer> output_buffer_{nil};
    id<MTLTexture> atlas_texture_{nil};
    std::uint64_t fill_capacity_{0U};
    std::uint64_t glyph_capacity_{0U};
    std::uint64_t output_capacity_{0U};
    std::uint32_t output_width_{0U};
    std::uint32_t output_height_{0U};
    std::uint32_t atlas_width_{0U};
    std::uint32_t atlas_height_{0U};
    std::uint32_t atlas_layers_{0U};
    std::vector<std::uint32_t> atlas_generations_;
    std::vector<std::uint64_t> atlas_checksums_;
    std::vector<std::uint16_t> atlas_widths_;
    std::vector<std::uint16_t> atlas_heights_;
};

} // namespace

std::unique_ptr<NativeShaderExecutor>
make_metal_native_shader_executor() noexcept {
    try {
        return std::make_unique<MetalShaderExecutor>();
    } catch (...) {
        return nullptr;
    }
}

} // namespace zevryon::text

#endif // ZEVRYON_HAS_METAL_SHADER_EXECUTION
