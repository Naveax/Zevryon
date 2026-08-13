#include "native_shader_execution.hpp"

#if defined(ZEVRYON_HAS_METAL_SHADER_EXECUTION)

#include "native_metal_window_context.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

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
#include <unordered_map>
#include <utility>
#include <vector>

namespace zevryon::text {
namespace {

using detail::MetalWindowSharedContext;

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::uint32_t kThreadWidth = 8U;
constexpr std::uint32_t kThreadHeight = 8U;

constexpr const char* kMetalSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct PushConstants {
    uint4 a;
    uint4 b;
    uint4 c;
    uint4 d;
    uint4 e;
    uint4 f;
};

uint channel_b(uint value) { return value & 255U; }
uint channel_g(uint value) { return (value >> 8U) & 255U; }
uint channel_r(uint value) { return (value >> 16U) & 255U; }
uint channel_a(uint value) { return (value >> 24U) & 255U; }

uint pack_bgra(uint b, uint g, uint r, uint a) {
    return b | (g << 8U) | (r << 16U) | (a << 24U);
}

uint mul_u8(uint left, uint right) {
    return (left * right + 127U) / 255U;
}

uint blend_channel(uint source, uint destination, uint source_alpha) {
    uint inverse = 255U - source_alpha;
    uint value = source * 255U + destination * inverse + 127U;
    return min(255U, value / 255U);
}

uint blend_pixel(uint destination, uint source) {
    uint alpha = channel_a(source);
    return pack_bgra(
        blend_channel(channel_b(source), channel_b(destination), alpha),
        blend_channel(channel_g(source), channel_g(destination), alpha),
        blend_channel(channel_r(source), channel_r(destination), alpha),
        blend_channel(alpha, channel_a(destination), alpha));
}

uint premultiply_coverage(uint color, uint coverage) {
    uint alpha = mul_u8(channel_a(color), coverage);
    return pack_bgra(
        mul_u8(channel_b(color), alpha),
        mul_u8(channel_g(color), alpha),
        mul_u8(channel_r(color), alpha),
        alpha);
}

kernel void zevryon_integer_compose(
    texture2d_array<uint, access::read> atlas [[texture(0)]],
    texture2d<uint, access::read_write> output [[texture(1)]],
    constant PushConstants& pc [[buffer(0)]],
    uint2 local [[thread_position_in_grid]]) {
    uint surface_width = pc.a.x;
    uint surface_height = pc.a.y;
    uint operation = pc.a.z;

    if (operation == 0U) {
        if (local.x < surface_width && local.y < surface_height) {
            output.write(uint4(0U), local);
        }
        return;
    }

    uint dispatch_width = pc.b.z;
    uint dispatch_height = pc.b.w;
    if (local.x >= dispatch_width || local.y >= dispatch_height) {
        return;
    }

    uint2 output_pixel = uint2(pc.b.x + local.x, pc.b.y + local.y);
    if (output_pixel.x >= surface_width || output_pixel.y >= surface_height) {
        return;
    }

    uint composed = output.read(output_pixel).x;
    int destination_x = int(pc.c.x);
    int destination_y = int(pc.c.y);
    int destination_width = int(pc.c.z);
    int destination_height = int(pc.c.w);

    if (operation == 1U) {
        composed = blend_pixel(composed, pc.e.y);
    } else if (operation == 2U) {
        uint local_x = uint(int(output_pixel.x) - destination_x);
        uint local_y = uint(int(output_pixel.y) - destination_y);
        uint source_x = pc.d.y + (local_x * pc.d.w) / uint(destination_width);
        uint source_y = pc.d.z + (local_y * pc.e.x) / uint(destination_height);
        uint texel = atlas.read(uint2(source_x, source_y), pc.d.x).x;
        uint color = pc.e.y;
        uint format = pc.e.z;
        uint source = 0U;
        if (format == 0U) {
            source = premultiply_coverage(color, channel_a(texel));
        } else if (format == 1U) {
            uint coverage_b = channel_b(texel);
            uint coverage_g = channel_g(texel);
            uint coverage_r = channel_r(texel);
            uint modulation_alpha = channel_a(color);
            uint alpha = mul_u8(
                modulation_alpha,
                max(coverage_b, max(coverage_g, coverage_r)));
            source = pack_bgra(
                mul_u8(mul_u8(channel_b(color), modulation_alpha), coverage_b),
                mul_u8(mul_u8(channel_g(color), modulation_alpha), coverage_g),
                mul_u8(mul_u8(channel_r(color), modulation_alpha), coverage_r),
                alpha);
        } else {
            uint modulation_alpha = channel_a(color);
            source = pack_bgra(
                mul_u8(channel_b(texel), modulation_alpha),
                mul_u8(channel_g(texel), modulation_alpha),
                mul_u8(channel_r(texel), modulation_alpha),
                mul_u8(channel_a(texel), modulation_alpha));
        }
        composed = blend_pixel(composed, source);
    }

    output.write(uint4(composed, 0U, 0U, 0U), output_pixel);
}
)METAL";

struct MetalPushConstants final {
    std::array<std::uint32_t, 4U> a{};
    std::array<std::uint32_t, 4U> b{};
    std::array<std::uint32_t, 4U> c{};
    std::array<std::uint32_t, 4U> d{};
    std::array<std::uint32_t, 4U> e{};
    std::array<std::uint32_t, 4U> f{};
};
static_assert(sizeof(MetalPushConstants) == 96U);

struct AtlasSignature final {
    std::uint32_t page_generation{0U};
    std::uint16_t width{0U};
    std::uint16_t height{0U};
    std::uint64_t checksum{0U};
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

std::uint64_t bytes_checksum(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_bytes(&hash, bytes);
    return hash;
}

std::uint64_t object_id(id object) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
        (__bridge void*)object));
}

std::uint32_t pack_color(ShaderColorBgra8 color) noexcept {
    return static_cast<std::uint32_t>(color.blue) |
        (static_cast<std::uint32_t>(color.green) << 8U) |
        (static_cast<std::uint32_t>(color.red) << 16U) |
        (static_cast<std::uint32_t>(color.alpha) << 24U);
}

bool intersect_rect(
    ShaderRectI left,
    ShaderRectI right,
    ShaderRectI* output) noexcept {
    if (output == nullptr || left.width <= 0 || left.height <= 0 ||
        right.width <= 0 || right.height <= 0) {
        return false;
    }
    const std::int64_t x0 = std::max<std::int64_t>(left.x, right.x);
    const std::int64_t y0 = std::max<std::int64_t>(left.y, right.y);
    const std::int64_t x1 = std::min<std::int64_t>(
        static_cast<std::int64_t>(left.x) + left.width,
        static_cast<std::int64_t>(right.x) + right.width);
    const std::int64_t y1 = std::min<std::int64_t>(
        static_cast<std::int64_t>(left.y) + left.height,
        static_cast<std::int64_t>(right.y) + right.height);
    if (x0 >= x1 || y0 >= y1 ||
        x0 < (std::numeric_limits<std::int32_t>::min)() ||
        y0 < (std::numeric_limits<std::int32_t>::min)() ||
        x1 > (std::numeric_limits<std::int32_t>::max)() ||
        y1 > (std::numeric_limits<std::int32_t>::max)()) {
        return false;
    }
    *output = {
        static_cast<std::int32_t>(x0),
        static_cast<std::int32_t>(y0),
        static_cast<std::int32_t>(x1 - x0),
        static_cast<std::int32_t>(y1 - y0)};
    return true;
}

bool validate_packet(
    const GpuShaderPacket& packet,
    const NativeShaderExecutionLimits& limits,
    NativeShaderExecutionError* error) noexcept {
    if (packet.header.frame_id == 0U ||
        packet.header.surface_width == 0U ||
        packet.header.surface_height == 0U ||
        packet.header.surface_width > limits.maximum_surface_width ||
        packet.header.surface_height > limits.maximum_surface_height ||
        packet.header.command_count != packet.commands.size() ||
        packet.header.fill_instance_count != packet.fills.size() ||
        packet.header.glyph_instance_count != packet.glyphs.size() ||
        packet.header.scissor_count != packet.scissors.size() ||
        packet.header.upload_count != packet.uploads.size() ||
        packet.header.command_count > limits.maximum_commands ||
        packet.header.fill_instance_count > limits.maximum_fill_instances ||
        packet.header.glyph_instance_count > limits.maximum_glyph_instances ||
        packet.header.packet_bytes > limits.maximum_packet_bytes ||
        shader_packet_checksum(packet) != packet.header.packet_checksum) {
        return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                    "invalid or corrupt Metal shader packet");
    }
    return true;
}

MTLSize threadgroups_for(std::uint32_t width, std::uint32_t height) noexcept {
    return MTLSizeMake(
        (width + kThreadWidth - 1U) / kThreadWidth,
        (height + kThreadHeight - 1U) / kThreadHeight,
        1U);
}

class MetalNativeShaderExecutor final : public NativeShaderExecutor {
public:
    ~MetalNativeShaderExecutor() override { shutdown(); }

    NativeGpuApiKind kind() const noexcept override {
        return NativeGpuApiKind::Metal;
    }

    bool configure(
        const NativeShaderExecutionConfig& config,
        NativeShaderExecutionError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        shutdown_locked();
        if (config.context.api_kind != NativeGpuApiKind::Metal ||
            config.executor_generation == 0U ||
            config.limits.maximum_commands == 0U ||
            config.limits.maximum_atlas_pages == 0U) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "invalid Metal shader execution configuration");
        }
        context_ = detail::retain_metal_window_context(config.context);
        if (context_ == nullptr) {
            return fail(error, NativeShaderExecutionErrorKind::NativeContextUnavailable,
                        "Metal retained context is unavailable");
        }
        @autoreleasepool {
            NSError* native_error = nil;
            NSString* source = [[NSString alloc] initWithUTF8String:kMetalSource];
            id<MTLLibrary> library = [context_->device
                newLibraryWithSource:source options:nil error:&native_error];
            if (library == nil) {
                const std::int64_t code = native_error == nil ? 0 : native_error.code;
                shutdown_locked();
                return fail(error, NativeShaderExecutionErrorKind::ShaderCompilationFailed,
                            "Metal integer shader compilation failed", code);
            }
            id<MTLFunction> function = [library newFunctionWithName:@"zevryon_integer_compose"];
            if (function == nil) {
                shutdown_locked();
                return fail(error, NativeShaderExecutionErrorKind::ShaderCompilationFailed,
                            "Metal integer shader entry point is missing");
            }
            pipeline_ = [context_->device
                newComputePipelineStateWithFunction:function error:&native_error];
            if (pipeline_ == nil) {
                const std::int64_t code = native_error == nil ? 0 : native_error.code;
                shutdown_locked();
                return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                            "Metal compute pipeline creation failed", code);
            }
        }
        config_ = config;
        snapshot_ = {};
        snapshot_.api_kind = NativeGpuApiKind::Metal;
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
        if (context_ == nullptr || pipeline_ == nil ||
            !validate_packet(packet, config_.limits, error)) {
            ++snapshot_.rejected_packets;
            return false;
        }
        try {
            if (!ensure_atlas(packet, atlas, error) ||
                !ensure_output(packet.header.surface_width,
                               packet.header.surface_height,
                               readback != nullptr,
                               error)) {
                return false;
            }
            @autoreleasepool {
                id<MTLCommandBuffer> command_buffer = [context_->queue commandBuffer];
                if (command_buffer == nil) {
                    return fail(error, NativeShaderExecutionErrorKind::SubmissionFailed,
                                "Metal command buffer allocation failed");
                }
                id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
                if (encoder == nil) {
                    return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                                "Metal compute encoder allocation failed");
                }
                [encoder setComputePipelineState:pipeline_];
                [encoder setTexture:atlas_texture_ atIndex:0U];
                [encoder setTexture:output_texture_ atIndex:1U];

                MetalPushConstants constants{};
                constants.a[0] = packet.header.surface_width;
                constants.a[1] = packet.header.surface_height;
                constants.a[2] = 0U;
                encode_dispatch(encoder, constants,
                    packet.header.surface_width, packet.header.surface_height);
                [encoder memoryBarrierWithScope:MTLBarrierScopeTextures];

                for (const GpuShaderDrawCommand& command : packet.commands) {
                    if (!encode_command(encoder, packet, command, error)) {
                        [encoder endEncoding];
                        return false;
                    }
                }
                [encoder endEncoding];

                if (readback != nullptr) {
                    id<MTLBlitCommandEncoder> blit =
                        [command_buffer blitCommandEncoder];
                    if (blit == nil) {
                        return fail(error, NativeShaderExecutionErrorKind::ReadbackFailed,
                                    "Metal readback blit encoder allocation failed");
                    }
                    [blit copyFromTexture:output_texture_
                              sourceSlice:0U
                              sourceLevel:0U
                             sourceOrigin:MTLOriginMake(0U, 0U, 0U)
                               sourceSize:MTLSizeMake(
                                   packet.header.surface_width,
                                   packet.header.surface_height,
                                   1U)
                                 toBuffer:readback_buffer_
                        destinationOffset:0U
                   destinationBytesPerRow:readback_row_bytes_
                 destinationBytesPerImage:readback_buffer_bytes_];
                    [blit endEncoding];
                }

                [command_buffer commit];
                [command_buffer waitUntilCompleted];
                if (command_buffer.status == MTLCommandBufferStatusError) {
                    const NSError* native_error = command_buffer.error;
                    const std::int64_t code = native_error == nil ? 0 : native_error.code;
                    return fail(error, NativeShaderExecutionErrorKind::DeviceLost,
                                "Metal shader command buffer failed", code);
                }
            }

            const std::uint64_t canonical_row_bytes =
                static_cast<std::uint64_t>(packet.header.surface_width) * 4U;
            std::uint64_t canonical_bytes = 0U;
            if (!checked_multiply(
                    canonical_row_bytes, packet.header.surface_height,
                    &canonical_bytes)) {
                return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                            "Metal output surface size overflowed");
            }

            if (readback != nullptr) {
                if (canonical_bytes > config_.limits.maximum_readback_bytes ||
                    canonical_bytes > (std::numeric_limits<std::size_t>::max)()) {
                    return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                                "Metal readback exceeds configured budget");
                }
                ShaderReadback staged;
                staged.width = packet.header.surface_width;
                staged.height = packet.header.surface_height;
                staged.row_bytes = static_cast<std::uint32_t>(canonical_row_bytes);
                staged.bgra.resize(static_cast<std::size_t>(canonical_bytes));
                const auto* source =
                    static_cast<const std::byte*>(readback_buffer_.contents);
                for (std::uint32_t row = 0U; row < staged.height; ++row) {
                    std::memcpy(
                        staged.bgra.data() +
                            static_cast<std::size_t>(row) * staged.row_bytes,
                        source + static_cast<std::size_t>(row) * readback_row_bytes_,
                        staged.row_bytes);
                }
                staged.checksum = shader_bytes_checksum(staged.bgra);
                *readback = std::move(staged);
                ++snapshot_.readbacks;
                snapshot_.last_readback_checksum = readback->checksum;
            }

            last_surface_ = {};
            last_surface_.api_kind = NativeGpuApiKind::Metal;
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
            last_surface_.native_resource = object_id(output_texture_);
            last_surface_.width = packet.header.surface_width;
            last_surface_.height = packet.header.surface_height;

            ++snapshot_.executions;
            snapshot_.last_packet_checksum = packet.header.packet_checksum;
            snapshot_.persistent_atlas_bytes = atlas.resident_bytes();
            snapshot_.output_surface_bytes = canonical_bytes;
            return true;
        } catch (const std::bad_alloc&) {
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "Metal shader execution allocation failed");
        } catch (...) {
            return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                        "unexpected Metal shader execution failure");
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
            output_texture_ == nil ||
            object_id(output_texture_) != last_surface_.native_resource) {
            *surface = {};
            return fail(error, NativeShaderExecutionErrorKind::StaleGeneration,
                        "no completed Metal shader surface is available");
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
        shutdown_locked();
    }

private:
    bool ensure_atlas(
        const GpuShaderPacket& packet,
        const ShaderAtlasResidency& atlas,
        NativeShaderExecutionError* error) noexcept {
        std::uint32_t maximum_page = 0U;
        std::uint16_t maximum_width = 1U;
        std::uint16_t maximum_height = 1U;
        std::unordered_map<std::uint32_t, const ShaderAtlasResidentPage*> pages;
        try {
            pages.reserve(packet.glyphs.size());
            for (const GpuShaderGlyphInstance& glyph : packet.glyphs) {
                const ShaderAtlasResidentPage* page = atlas.find(
                    glyph.atlas_page_index, glyph.atlas_page_generation);
                if (page == nullptr || page->canonical_bgra.empty()) {
                    return fail(error, NativeShaderExecutionErrorKind::StaleGeneration,
                                "Metal atlas page is missing or stale");
                }
                pages.emplace(glyph.atlas_page_index, page);
                maximum_page = std::max(maximum_page, glyph.atlas_page_index);
                maximum_width = std::max(maximum_width, page->width);
                maximum_height = std::max(maximum_height, page->height);
            }
        } catch (...) {
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "Metal atlas metadata allocation failed");
        }
        if (maximum_page >= config_.limits.maximum_atlas_pages) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "Metal atlas page index exceeds configured limit");
        }
        const std::uint32_t layers = std::max(1U, maximum_page + 1U);
        const bool recreate = atlas_texture_ == nil ||
            atlas_width_ < maximum_width || atlas_height_ < maximum_height ||
            atlas_layers_ < layers;
        if (recreate) {
            MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
            descriptor.textureType = MTLTextureType2DArray;
            descriptor.pixelFormat = MTLPixelFormatR32Uint;
            descriptor.width = maximum_width;
            descriptor.height = maximum_height;
            descriptor.arrayLength = layers;
            descriptor.mipmapLevelCount = 1U;
            descriptor.usage = MTLTextureUsageShaderRead;
            descriptor.storageMode = context_->device.hasUnifiedMemory
                ? MTLStorageModeShared : MTLStorageModeManaged;
            id<MTLTexture> staged =
                [context_->device newTextureWithDescriptor:descriptor];
            if (staged == nil) {
                return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                            "Metal persistent atlas texture allocation failed");
            }
            atlas_texture_ = staged;
            atlas_width_ = maximum_width;
            atlas_height_ = maximum_height;
            atlas_layers_ = layers;
            atlas_signatures_.clear();
        }

        bool uploaded = false;
        for (const auto& entry : pages) {
            const std::uint32_t page_index = entry.first;
            const ShaderAtlasResidentPage& page = *entry.second;
            const AtlasSignature signature{
                page.page_generation, page.width, page.height,
                bytes_checksum(page.canonical_bgra)};
            const auto found = atlas_signatures_.find(page_index);
            if (found != atlas_signatures_.end() &&
                found->second.page_generation == signature.page_generation &&
                found->second.width == signature.width &&
                found->second.height == signature.height &&
                found->second.checksum == signature.checksum) {
                continue;
            }
            [atlas_texture_ replaceRegion:MTLRegionMake2D(
                    0U, 0U, page.width, page.height)
                               mipmapLevel:0U
                                     slice:page_index
                                 withBytes:page.canonical_bgra.data()
                               bytesPerRow:static_cast<NSUInteger>(page.width) * 4U
                             bytesPerImage:static_cast<NSUInteger>(page.width) *
                                           static_cast<NSUInteger>(page.height) * 4U];
            atlas_signatures_[page_index] = signature;
            uploaded = true;
        }
        if (uploaded) {
            ++snapshot_.atlas_upload_batches;
        } else if (!pages.empty()) {
            ++snapshot_.atlas_reuses;
        }
        snapshot_.persistent_atlas_bytes = atlas.resident_bytes();
        return true;
    }

    bool ensure_output(
        std::uint32_t width,
        std::uint32_t height,
        bool require_readback,
        NativeShaderExecutionError* error) noexcept {
        std::uint64_t canonical_bytes = 0U;
        if (!checked_multiply(
                static_cast<std::uint64_t>(width) * 4U,
                height, &canonical_bytes)) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "Metal output surface size overflowed");
        }

        const bool output_matches = output_texture_ != nil &&
            output_width_ == width && output_height_ == height;
        if (!output_matches) {
            if (next_output_generation_ ==
                (std::numeric_limits<std::uint64_t>::max)()) {
                return fail(error,
                            NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                            "Metal shader output generation overflowed");
            }
            MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint
                                             width:width
                                            height:height
                                         mipmapped:NO];
            descriptor.usage =
                MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            descriptor.storageMode = MTLStorageModePrivate;
            id<MTLTexture> texture =
                [context_->device newTextureWithDescriptor:descriptor];
            if (texture == nil) {
                return fail(error,
                            NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                            "Metal output texture allocation failed");
            }
            output_texture_ = texture;
            output_width_ = width;
            output_height_ = height;
            output_generation_ = next_output_generation_++;
            last_surface_ = {};
            snapshot_.output_surface_bytes = canonical_bytes;
        }

        if (!require_readback) {
            return true;
        }
        return ensure_readback(width, height, error);
    }

    bool ensure_readback(
        std::uint32_t width,
        std::uint32_t height,
        NativeShaderExecutionError* error) noexcept {
        const std::uint64_t aligned_row =
            (static_cast<std::uint64_t>(width) * 4U + 255U) & ~255ULL;
        std::uint64_t aligned_bytes = 0U;
        if (!checked_multiply(aligned_row, height, &aligned_bytes) ||
            aligned_bytes > config_.limits.maximum_readback_bytes ||
            aligned_bytes > (std::numeric_limits<NSUInteger>::max)()) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "Metal aligned readback exceeds configured budget");
        }
        if (readback_buffer_ != nil &&
            readback_row_bytes_ == static_cast<NSUInteger>(aligned_row) &&
            readback_buffer_bytes_ >= static_cast<NSUInteger>(aligned_bytes)) {
            return true;
        }

        id<MTLBuffer> buffer = [context_->device
            newBufferWithLength:static_cast<NSUInteger>(aligned_bytes)
                         options:MTLResourceStorageModeShared];
        if (buffer == nil) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                        "Metal readback buffer allocation failed");
        }
        readback_buffer_ = buffer;
        readback_row_bytes_ = static_cast<NSUInteger>(aligned_row);
        readback_buffer_bytes_ = static_cast<NSUInteger>(aligned_bytes);
        snapshot_.peak_transient_bytes = std::max<std::uint64_t>(
            snapshot_.peak_transient_bytes, aligned_bytes);
        return true;
    }

    void encode_dispatch(
        id<MTLComputeCommandEncoder> encoder,
        const MetalPushConstants& constants,
        std::uint32_t width,
        std::uint32_t height) noexcept {
        [encoder setBytes:&constants length:sizeof(constants) atIndex:0U];
        [encoder dispatchThreadgroups:threadgroups_for(width, height)
                threadsPerThreadgroup:MTLSizeMake(
                    kThreadWidth, kThreadHeight, 1U)];
    }

    bool encode_command(
        id<MTLComputeCommandEncoder> encoder,
        const GpuShaderPacket& packet,
        const GpuShaderDrawCommand& command,
        NativeShaderExecutionError* error) noexcept {
        if (command.scissor_index >= packet.scissors.size()) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "Metal command references invalid scissor");
        }
        const ShaderRectI scissor = packet.scissors[command.scissor_index].rect;
        if (command.kind == ShaderPrimitiveKind::Fill) {
            if (command.first_instance > packet.fills.size() ||
                command.instance_count > packet.fills.size() - command.first_instance) {
                return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                            "Metal fill command range is invalid");
            }
            for (std::uint32_t index = 0U; index < command.instance_count; ++index) {
                const GpuShaderFillInstance& fill =
                    packet.fills[command.first_instance + index];
                ShaderRectI clipped{};
                if (!intersect_rect(fill.destination, scissor, &clipped)) {
                    continue;
                }
                MetalPushConstants constants{};
                constants.a = {packet.header.surface_width,
                    packet.header.surface_height, 1U, 0U};
                constants.b = {static_cast<std::uint32_t>(clipped.x),
                    static_cast<std::uint32_t>(clipped.y),
                    static_cast<std::uint32_t>(clipped.width),
                    static_cast<std::uint32_t>(clipped.height)};
                constants.c = {static_cast<std::uint32_t>(fill.destination.x),
                    static_cast<std::uint32_t>(fill.destination.y),
                    static_cast<std::uint32_t>(fill.destination.width),
                    static_cast<std::uint32_t>(fill.destination.height)};
                constants.e[1] = pack_color(fill.color);
                encode_dispatch(encoder, constants,
                    static_cast<std::uint32_t>(clipped.width),
                    static_cast<std::uint32_t>(clipped.height));
                [encoder memoryBarrierWithScope:MTLBarrierScopeTextures];
            }
            return true;
        }
        if (command.first_instance > packet.glyphs.size() ||
            command.instance_count > packet.glyphs.size() - command.first_instance) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "Metal glyph command range is invalid");
        }
        for (std::uint32_t index = 0U; index < command.instance_count; ++index) {
            const GpuShaderGlyphInstance& glyph =
                packet.glyphs[command.first_instance + index];
            ShaderRectI clipped{};
            if (!intersect_rect(glyph.destination, scissor, &clipped)) {
                continue;
            }
            MetalPushConstants constants{};
            constants.a = {packet.header.surface_width,
                packet.header.surface_height, 2U, 0U};
            constants.b = {static_cast<std::uint32_t>(clipped.x),
                static_cast<std::uint32_t>(clipped.y),
                static_cast<std::uint32_t>(clipped.width),
                static_cast<std::uint32_t>(clipped.height)};
            constants.c = {static_cast<std::uint32_t>(glyph.destination.x),
                static_cast<std::uint32_t>(glyph.destination.y),
                static_cast<std::uint32_t>(glyph.destination.width),
                static_cast<std::uint32_t>(glyph.destination.height)};
            constants.d = {glyph.atlas_page_index, glyph.atlas_x,
                glyph.atlas_y, glyph.atlas_width};
            constants.e = {glyph.atlas_height, pack_color(glyph.color),
                static_cast<std::uint32_t>(glyph.format), 0U};
            encode_dispatch(encoder, constants,
                static_cast<std::uint32_t>(clipped.width),
                static_cast<std::uint32_t>(clipped.height));
            [encoder memoryBarrierWithScope:MTLBarrierScopeTextures];
        }
        return true;
    }

    void shutdown_locked() noexcept {
        readback_buffer_ = nil;
        output_texture_ = nil;
        atlas_texture_ = nil;
        pipeline_ = nil;
        atlas_signatures_.clear();
        atlas_width_ = 0U;
        atlas_height_ = 0U;
        atlas_layers_ = 0U;
        output_width_ = 0U;
        output_height_ = 0U;
        readback_row_bytes_ = 0U;
        readback_buffer_bytes_ = 0U;
        output_generation_ = 0U;
        next_output_generation_ = 1U;
        last_surface_ = {};
        if (context_ != nullptr) {
            detail::release_metal_window_context(context_);
            context_ = nullptr;
        }
        config_ = {};
        snapshot_ = {};
    }

    mutable std::mutex mutex_;
    NativeShaderExecutionConfig config_{};
    NativeShaderExecutionSnapshot snapshot_{};
    MetalWindowSharedContext* context_{nullptr};
    id<MTLComputePipelineState> pipeline_{nil};
    id<MTLTexture> atlas_texture_{nil};
    id<MTLTexture> output_texture_{nil};
    id<MTLBuffer> readback_buffer_{nil};
    std::unordered_map<std::uint32_t, AtlasSignature> atlas_signatures_;
    std::uint16_t atlas_width_{0U};
    std::uint16_t atlas_height_{0U};
    std::uint32_t atlas_layers_{0U};
    std::uint32_t output_width_{0U};
    std::uint32_t output_height_{0U};
    NSUInteger readback_row_bytes_{0U};
    NSUInteger readback_buffer_bytes_{0U};
    std::uint64_t output_generation_{0U};
    std::uint64_t next_output_generation_{1U};
    NativeShaderSurfaceView last_surface_{};
};

} // namespace

std::unique_ptr<NativeShaderExecutor>
make_metal_native_shader_executor() noexcept {
    try {
        return std::make_unique<MetalNativeShaderExecutor>();
    } catch (...) {
        return nullptr;
    }
}

} // namespace zevryon::text

#endif // ZEVRYON_HAS_METAL_SHADER_EXECUTION
