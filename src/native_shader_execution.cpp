#include "native_shader_execution.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

namespace zevryon::text {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

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

void clear_error(NativeShaderExecutionError* error) noexcept {
    if (error != nullptr) {
        error->kind = NativeShaderExecutionErrorKind::None;
        error->native_code = 0;
        error->message.clear();
    }
}

bool add_u64(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t* output) noexcept {
    if (output == nullptr ||
        right > (std::numeric_limits<std::uint64_t>::max)() - left) {
        return false;
    }
    *output = left + right;
    return true;
}

bool multiply_u64(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t* output) noexcept {
    if (output == nullptr ||
        (left != 0U &&
         right > (std::numeric_limits<std::uint64_t>::max)() / left)) {
        return false;
    }
    *output = left * right;
    return true;
}

template <typename T>
void hash_value(std::uint64_t* hash, const T& value) noexcept {
    const auto bytes = std::as_bytes(std::span<const T>(&value, 1U));
    for (const std::byte byte : bytes) {
        *hash ^= static_cast<std::uint8_t>(byte);
        *hash *= kFnvPrime;
    }
}

void hash_text(std::uint64_t* hash, std::string_view text) noexcept {
    for (const unsigned char byte : text) {
        *hash ^= byte;
        *hash *= kFnvPrime;
    }
}

bool context_valid(
    NativeGpuApiKind kind,
    const NativeGpuSdkContextHandle& context) noexcept {
    if (kind == NativeGpuApiKind::ReferenceCpu) {
        return context.api_kind == NativeGpuApiKind::ReferenceCpu ||
            context.api_kind == kind;
    }
    const std::uint32_t required =
        kNativeGpuSdkContextDeviceValid |
        kNativeGpuSdkContextGraphicsQueueValid;
    return context.api_kind == kind &&
        context.device_generation != 0U &&
        context.runtime_generation != 0U &&
        context.device != 0U &&
        context.graphics_queue != 0U &&
        (context.flags & required) == required;
}

class ReferenceNativeShaderExecutionApi final
    : public NativeShaderExecutionApi {
public:
    NativeGpuApiKind kind() const noexcept override {
        return NativeGpuApiKind::ReferenceCpu;
    }

    NativeShaderCapabilities capabilities() const noexcept override {
        return default_native_shader_capabilities(NativeGpuApiKind::ReferenceCpu);
    }

    bool configure(
        const NativeGpuSdkContextHandle& context,
        const NativeShaderExecutionLimits& limits,
        NativeShaderExecutionError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (!context_valid(NativeGpuApiKind::ReferenceCpu, context) ||
            limits.maximum_commands == 0U ||
            limits.maximum_scissors == 0U ||
            limits.maximum_fill_instances == 0U ||
            limits.maximum_glyph_instances == 0U ||
            limits.maximum_atlas_pages == 0U ||
            limits.maximum_frames_in_flight == 0U ||
            limits.maximum_surface_width == 0U ||
            limits.maximum_surface_height == 0U ||
            limits.maximum_packet_bytes == 0U ||
            limits.maximum_atlas_bytes == 0U ||
            limits.maximum_output_bytes == 0U) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "invalid reference shader execution configuration");
        }
        snapshot_ = {};
        snapshot_.capabilities = capabilities();
        snapshot_.limits = limits;
        snapshot_.context = context;
        snapshot_.configurations = 1U;
        configured_ = true;
        next_fence_value_ = 1U;
        return true;
    }

    bool execute(
        const NativeShaderExecutionRequest& request,
        ShaderReadback* readback,
        NativeShaderExecutionReceipt* receipt,
        NativeShaderExecutionError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (!configured_ || request.packet == nullptr || request.atlas == nullptr ||
            receipt == nullptr || request.ticket_id == 0U) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "invalid reference shader execution request");
        }
        if (snapshot_.in_flight_count >=
            snapshot_.limits.maximum_frames_in_flight) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "reference shader frames-in-flight limit exceeded");
        }

        NativeShaderDispatchPlan plan;
        if (!compile_native_shader_dispatch_plan(
                NativeGpuApiKind::ReferenceCpu,
                *request.packet,
                *request.atlas,
                snapshot_.limits,
                &plan,
                error)) {
            return false;
        }

        ShaderReadback candidate;
        ShaderPacketError packet_error;
        if (!execute_shader_packet_reference(
                *request.packet, *request.atlas, &candidate, &packet_error)) {
            return fail(error, NativeShaderExecutionErrorKind::ReadbackFailed,
                        packet_error.message.c_str());
        }
        if ((request.flags & kNativeShaderExecutionRequireExactReadback) != 0U &&
            request.expected_readback_checksum != 0U &&
            candidate.checksum != request.expected_readback_checksum) {
            return fail(error, NativeShaderExecutionErrorKind::ReadbackMismatch,
                        "reference shader readback checksum differs from expectation");
        }

        const std::uint64_t signal = next_fence_value_++;
        if (signal <= snapshot_.last_submitted_fence_value) {
            return fail(error, NativeShaderExecutionErrorKind::SubmissionFailed,
                        "reference shader fence timeline regressed");
        }
        *receipt = {};
        receipt->api_kind = NativeGpuApiKind::ReferenceCpu;
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
        receipt->readback_checksum = candidate.checksum;
        receipt->output_bytes = plan.header.output_bytes;

        snapshot_.executions += 1U;
        snapshot_.readbacks += 1U;
        snapshot_.last_submitted_fence_value = signal;
        snapshot_.in_flight_count += 1U;
        snapshot_.current_device_bytes = plan.header.output_bytes;
        if (plan.header.atlas_bytes >
                (std::numeric_limits<std::uint64_t>::max)() -
                    snapshot_.current_device_bytes) {
            snapshot_.current_device_bytes =
                (std::numeric_limits<std::uint64_t>::max)();
        } else {
            snapshot_.current_device_bytes += plan.header.atlas_bytes;
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
        snapshot_.resident_atlas_pages = plan.header.atlas_binding_count;

        if (readback != nullptr) {
            *readback = std::move(candidate);
        }
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
                        "reference shader completion fence is outside timeline");
        }
        if (completed_fence_value > snapshot_.completed_fence_value) {
            const std::uint64_t delta =
                completed_fence_value - snapshot_.completed_fence_value;
            const std::uint32_t retired = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(delta, snapshot_.in_flight_count));
            snapshot_.in_flight_count -= retired;
            snapshot_.completed_fence_value = completed_fence_value;
            if (snapshot_.in_flight_count == 0U) {
                snapshot_.current_staging_bytes = 0U;
            }
        }
        return true;
    }

    NativeShaderExecutionSnapshot snapshot() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

    void shutdown() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = {};
        configured_ = false;
        next_fence_value_ = 1U;
    }

private:
    mutable std::mutex mutex_;
    NativeShaderExecutionSnapshot snapshot_;
    std::uint64_t next_fence_value_{1U};
    bool configured_{false};
};

constexpr std::string_view kHlslSource = R"HLSL(
// Z2F-8B3B2 exact integer compute compositor. Binding ABI is certified in C++.
struct Constants { uint width; uint height; uint commandCount; uint atlasLayers; uint2 frameId; uint2 packetChecksum; };
cbuffer FrameConstants : register(b0) { Constants frame; };
struct RectI { int x; int y; int width; int height; };
struct DrawCommand { uint kind; uint layer; uint atlasFormat; uint scissorIndex; uint firstInstance; uint instanceCount; uint atlasPageIndex; uint stableId; uint4 reserved; };
struct FillInstance { RectI destination; uint packedColor; uint scissorIndex; uint stableId; uint reserved; };
struct GlyphInstance { RectI destination; uint scissorIndex; uint atlasPageIndex; uint atlasPageGeneration; uint atlasXY; uint atlasWH; uint packedColor; uint format; uint stableId; uint4 reserved; };
struct Scissor { RectI rect; };
struct AtlasMeta { uint pageIndex; uint generation; uint layer; uint dimensions; uint rowBytes; uint3 reserved; };
StructuredBuffer<DrawCommand> commands : register(t0);
StructuredBuffer<FillInstance> fills : register(t1);
StructuredBuffer<GlyphInstance> glyphs : register(t2);
StructuredBuffer<Scissor> scissors : register(t3);
StructuredBuffer<AtlasMeta> atlasMeta : register(t4);
Texture2DArray<uint4> atlasTexture : register(t5);
RWStructuredBuffer<uint> outputPixels : register(u0);
uint mul8(uint a, uint b) { return (a * b + 127u) / 255u; }
uint4 unpackBgra(uint value) { return uint4(value & 255u, (value >> 8u) & 255u, (value >> 16u) & 255u, (value >> 24u) & 255u); }
uint packBgra(uint4 value) { return value.x | (value.y << 8u) | (value.z << 16u) | (value.w << 24u); }
uint4 blend(uint4 destination, uint4 source) {
    uint inverse = 255u - source.w;
    return uint4(min(255u, source.x + mul8(destination.x, inverse)), min(255u, source.y + mul8(destination.y, inverse)), min(255u, source.z + mul8(destination.z, inverse)), min(255u, source.w + mul8(destination.w, inverse)));
}
bool inside(RectI rect, int2 p) { return p.x >= rect.x && p.y >= rect.y && p.x < rect.x + rect.width && p.y < rect.y + rect.height; }
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= frame.width || id.y >= frame.height) return;
    int2 pixel = int2(id.xy);
    uint4 destination = uint4(0u, 0u, 0u, 0u);
    for (uint commandIndex = 0u; commandIndex < frame.commandCount; ++commandIndex) {
        DrawCommand command = commands[commandIndex];
        RectI clip = scissors[command.scissorIndex].rect;
        if (!inside(clip, pixel)) continue;
        if (command.kind == 0u) {
            for (uint index = 0u; index < command.instanceCount; ++index) {
                FillInstance fill = fills[command.firstInstance + index];
                if (inside(fill.destination, pixel)) destination = blend(destination, unpackBgra(fill.packedColor));
            }
        } else {
            for (uint index = 0u; index < command.instanceCount; ++index) {
                GlyphInstance glyph = glyphs[command.firstInstance + index];
                if (!inside(glyph.destination, pixel)) continue;
                uint atlasX = glyph.atlasXY & 65535u;
                uint atlasY = glyph.atlasXY >> 16u;
                uint atlasW = glyph.atlasWH & 65535u;
                uint atlasH = glyph.atlasWH >> 16u;
                uint localX = uint(pixel.x - glyph.destination.x);
                uint localY = uint(pixel.y - glyph.destination.y);
                uint sourceX = atlasX + (localX * atlasW) / uint(glyph.destination.width);
                uint sourceY = atlasY + (localY * atlasH) / uint(glyph.destination.height);
                uint4 texel = atlasTexture.Load(int4(sourceX, sourceY, glyph.atlasPageIndex, 0));
                uint4 color = unpackBgra(glyph.packedColor);
                uint4 source;
                if (glyph.format == 0u) {
                    source = uint4(mul8(mul8(color.x, color.w), texel.w), mul8(mul8(color.y, color.w), texel.w), mul8(mul8(color.z, color.w), texel.w), mul8(color.w, texel.w));
                } else if (glyph.format == 1u) {
                    uint alpha = mul8(color.w, max(texel.x, max(texel.y, texel.z)));
                    source = uint4(mul8(mul8(color.x, color.w), texel.x), mul8(mul8(color.y, color.w), texel.y), mul8(mul8(color.z, color.w), texel.z), alpha);
                } else {
                    source = uint4(mul8(texel.x, color.w), mul8(texel.y, color.w), mul8(texel.z, color.w), mul8(texel.w, color.w));
                }
                destination = blend(destination, source);
            }
        }
    }
    outputPixels[id.y * frame.width + id.x] = packBgra(destination);
}
)HLSL";

constexpr std::string_view kGlslSource = R"GLSL(
#version 450
// Generated SPIR-V is checked in after CI compilation; source remains canonical.
layout(local_size_x=8, local_size_y=8, local_size_z=1) in;
layout(std140, set=0, binding=0) uniform FrameConstants { uvec4 dimensions; uvec4 identities; } frame;
struct DrawCommand { uint kind; uint layer; uint atlasFormat; uint scissorIndex; uint firstInstance; uint instanceCount; uint atlasPageIndex; uint stableId; uint4 reserved; };
struct FillInstance { ivec4 destination; uint packedColor; uint scissorIndex; uint stableId; uint reserved; };
struct GlyphInstance { ivec4 destination; uint scissorIndex; uint atlasPageIndex; uint atlasPageGeneration; uint atlasXY; uint atlasWH; uint packedColor; uint format; uint stableId; uvec4 reserved; };
layout(std430, set=0, binding=1) readonly buffer Commands { DrawCommand values[]; } commands;
layout(std430, set=0, binding=2) readonly buffer Fills { FillInstance values[]; } fills;
layout(std430, set=0, binding=3) readonly buffer Glyphs { GlyphInstance values[]; } glyphs;
layout(std430, set=0, binding=4) readonly buffer Scissors { ivec4 values[]; } scissors;
layout(set=0, binding=5) uniform usampler2DArray atlasTexture;
layout(std430, set=0, binding=6) writeonly buffer OutputPixels { uint values[]; } outputPixels;
uint mul8(uint a, uint b) { return (a * b + 127u) / 255u; }
uvec4 unpackBgra(uint value) { return uvec4(value & 255u, (value >> 8u) & 255u, (value >> 16u) & 255u, (value >> 24u) & 255u); }
uint packBgra(uvec4 value) { return value.x | (value.y << 8u) | (value.z << 16u) | (value.w << 24u); }
uvec4 blendPixel(uvec4 destination, uvec4 source) { uint inverse = 255u - source.w; return uvec4(min(255u, source.x + mul8(destination.x, inverse)), min(255u, source.y + mul8(destination.y, inverse)), min(255u, source.z + mul8(destination.z, inverse)), min(255u, source.w + mul8(destination.w, inverse))); }
bool insideRect(ivec4 rect, ivec2 p) { return p.x >= rect.x && p.y >= rect.y && p.x < rect.x + rect.z && p.y < rect.y + rect.w; }
void main() {
    uvec2 id = gl_GlobalInvocationID.xy;
    if (id.x >= frame.dimensions.x || id.y >= frame.dimensions.y) return;
    ivec2 pixel = ivec2(id);
    uvec4 destination = uvec4(0u);
    for (uint commandIndex = 0u; commandIndex < frame.dimensions.z; ++commandIndex) {
        DrawCommand command = commands.values[commandIndex];
        ivec4 clip = scissors.values[command.scissorIndex];
        if (!insideRect(clip, pixel)) continue;
        if (command.kind == 0u) {
            for (uint index = 0u; index < command.instanceCount; ++index) {
                FillInstance fill = fills.values[command.firstInstance + index];
                if (insideRect(fill.destination, pixel)) destination = blendPixel(destination, unpackBgra(fill.packedColor));
            }
        } else {
            for (uint index = 0u; index < command.instanceCount; ++index) {
                GlyphInstance glyph = glyphs.values[command.firstInstance + index];
                if (!insideRect(glyph.destination, pixel)) continue;
                uint atlasX = glyph.atlasXY & 65535u; uint atlasY = glyph.atlasXY >> 16u;
                uint atlasW = glyph.atlasWH & 65535u; uint atlasH = glyph.atlasWH >> 16u;
                uint localX = uint(pixel.x - glyph.destination.x); uint localY = uint(pixel.y - glyph.destination.y);
                uint sourceX = atlasX + (localX * atlasW) / uint(glyph.destination.z);
                uint sourceY = atlasY + (localY * atlasH) / uint(glyph.destination.w);
                uvec4 texel = texelFetch(atlasTexture, ivec3(int(sourceX), int(sourceY), int(glyph.atlasPageIndex)), 0);
                uvec4 color = unpackBgra(glyph.packedColor); uvec4 source;
                if (glyph.format == 0u) source = uvec4(mul8(mul8(color.x, color.w), texel.w), mul8(mul8(color.y, color.w), texel.w), mul8(mul8(color.z, color.w), texel.w), mul8(color.w, texel.w));
                else if (glyph.format == 1u) { uint alpha = mul8(color.w, max(texel.x, max(texel.y, texel.z))); source = uvec4(mul8(mul8(color.x, color.w), texel.x), mul8(mul8(color.y, color.w), texel.y), mul8(mul8(color.z, color.w), texel.z), alpha); }
                else source = uvec4(mul8(texel.x, color.w), mul8(texel.y, color.w), mul8(texel.z, color.w), mul8(texel.w, color.w));
                destination = blendPixel(destination, source);
            }
        }
    }
    outputPixels.values[id.y * frame.dimensions.x + id.x] = packBgra(destination);
}
)GLSL";

constexpr std::string_view kMslSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Constants { uint width; uint height; uint commandCount; uint atlasLayers; ulong frameId; ulong packetChecksum; };
struct DrawCommand { uint kind; uint layer; uint atlasFormat; uint scissorIndex; uint firstInstance; uint instanceCount; uint atlasPageIndex; uint stableId; uint4 reserved; };
struct FillInstance { int4 destination; uint packedColor; uint scissorIndex; uint stableId; uint reserved; };
struct GlyphInstance { int4 destination; uint scissorIndex; uint atlasPageIndex; uint atlasPageGeneration; uint atlasXY; uint atlasWH; uint packedColor; uint format; uint stableId; uint4 reserved; };
uint mul8(uint a, uint b) { return (a * b + 127u) / 255u; }
uint4 unpackBgra(uint value) { return uint4(value & 255u, (value >> 8u) & 255u, (value >> 16u) & 255u, (value >> 24u) & 255u); }
uint packBgra(uint4 value) { return value.x | (value.y << 8u) | (value.z << 16u) | (value.w << 24u); }
uint4 blendPixel(uint4 destination, uint4 source) { uint inverse = 255u - source.w; return uint4(min(255u, source.x + mul8(destination.x, inverse)), min(255u, source.y + mul8(destination.y, inverse)), min(255u, source.z + mul8(destination.z, inverse)), min(255u, source.w + mul8(destination.w, inverse))); }
bool insideRect(int4 rect, int2 p) { return p.x >= rect.x && p.y >= rect.y && p.x < rect.x + rect.z && p.y < rect.y + rect.w; }
kernel void zevryon_shader_main(
    constant Constants& frame [[buffer(0)]],
    device const DrawCommand* commands [[buffer(1)]],
    device const FillInstance* fills [[buffer(2)]],
    device const GlyphInstance* glyphs [[buffer(3)]],
    device const int4* scissors [[buffer(4)]],
    texture2d_array<uint, access::read> atlasTexture [[texture(0)]],
    device uint* outputPixels [[buffer(5)]],
    uint2 id [[thread_position_in_grid]]) {
    if (id.x >= frame.width || id.y >= frame.height) return;
    int2 pixel = int2(id); uint4 destination = uint4(0u);
    for (uint commandIndex = 0u; commandIndex < frame.commandCount; ++commandIndex) {
        DrawCommand command = commands[commandIndex]; int4 clip = scissors[command.scissorIndex]; if (!insideRect(clip, pixel)) continue;
        if (command.kind == 0u) {
            for (uint index = 0u; index < command.instanceCount; ++index) { FillInstance fill = fills[command.firstInstance + index]; if (insideRect(fill.destination, pixel)) destination = blendPixel(destination, unpackBgra(fill.packedColor)); }
        } else {
            for (uint index = 0u; index < command.instanceCount; ++index) {
                GlyphInstance glyph = glyphs[command.firstInstance + index]; if (!insideRect(glyph.destination, pixel)) continue;
                uint atlasX = glyph.atlasXY & 65535u; uint atlasY = glyph.atlasXY >> 16u; uint atlasW = glyph.atlasWH & 65535u; uint atlasH = glyph.atlasWH >> 16u;
                uint localX = uint(pixel.x - glyph.destination.x); uint localY = uint(pixel.y - glyph.destination.y);
                uint sourceX = atlasX + (localX * atlasW) / uint(glyph.destination.z); uint sourceY = atlasY + (localY * atlasH) / uint(glyph.destination.w);
                uint4 texel = atlasTexture.read(uint2(sourceX, sourceY), glyph.atlasPageIndex); uint4 color = unpackBgra(glyph.packedColor); uint4 source;
                if (glyph.format == 0u) source = uint4(mul8(mul8(color.x, color.w), texel.w), mul8(mul8(color.y, color.w), texel.w), mul8(mul8(color.z, color.w), texel.w), mul8(color.w, texel.w));
                else if (glyph.format == 1u) { uint alpha = mul8(color.w, max(texel.x, max(texel.y, texel.z))); source = uint4(mul8(mul8(color.x, color.w), texel.x), mul8(mul8(color.y, color.w), texel.y), mul8(mul8(color.z, color.w), texel.z), alpha); }
                else source = uint4(mul8(texel.x, color.w), mul8(texel.y, color.w), mul8(texel.z, color.w), mul8(texel.w, color.w));
                destination = blendPixel(destination, source);
            }
        }
    }
    outputPixels[id.y * frame.width + id.x] = packBgra(destination);
}
)MSL";

} // namespace

NativeShaderDispatchPlan::NativeShaderDispatchPlan(
    std::pmr::memory_resource* resource)
    : commands(resource),
      fills(resource),
      glyphs(resource),
      scissors(resource),
      atlas_bindings(resource) {}

void NativeShaderDispatchPlan::clear() noexcept {
    header = {};
    bindings = {};
    constants = {};
    commands.clear();
    fills.clear();
    glyphs.clear();
    scissors.clear();
    atlas_bindings.clear();
}

const char* native_shader_execution_error_kind_name(
    NativeShaderExecutionErrorKind kind) noexcept {
    switch (kind) {
        case NativeShaderExecutionErrorKind::None: return "none";
        case NativeShaderExecutionErrorKind::InvalidInput: return "invalid-input";
        case NativeShaderExecutionErrorKind::UnsupportedBackend: return "unsupported-backend";
        case NativeShaderExecutionErrorKind::StaleGeneration: return "stale-generation";
        case NativeShaderExecutionErrorKind::InvalidPacket: return "invalid-packet";
        case NativeShaderExecutionErrorKind::InvalidAtlasReference: return "invalid-atlas-reference";
        case NativeShaderExecutionErrorKind::ResourceBudgetExceeded: return "resource-budget-exceeded";
        case NativeShaderExecutionErrorKind::AllocationFailed: return "allocation-failed";
        case NativeShaderExecutionErrorKind::ShaderCompilationFailed: return "shader-compilation-failed";
        case NativeShaderExecutionErrorKind::PipelineCreationFailed: return "pipeline-creation-failed";
        case NativeShaderExecutionErrorKind::CommandEncodingFailed: return "command-encoding-failed";
        case NativeShaderExecutionErrorKind::SubmissionFailed: return "submission-failed";
        case NativeShaderExecutionErrorKind::ReadbackFailed: return "readback-failed";
        case NativeShaderExecutionErrorKind::ReadbackMismatch: return "readback-mismatch";
        case NativeShaderExecutionErrorKind::DeviceLost: return "device-lost";
    }
    return "unknown";
}

NativeShaderExecutionLimits default_native_shader_execution_limits() noexcept {
    return NativeShaderExecutionLimits{
        256U,
        64U,
        256U,
        1024U,
        16U,
        3U,
        4096U,
        4096U,
        4U * 1024U * 1024U,
        16U * 1024U * 1024U,
        64U * 1024U * 1024U,
        16U * 1024U * 1024U};
}

NativeShaderCapabilities default_native_shader_capabilities(
    NativeGpuApiKind kind) noexcept {
    NativeShaderCapabilities result;
    result.api_kind = kind;
    result.flags = kNativeShaderComputePipeline |
        kNativeShaderPersistentAtlas |
        kNativeShaderExactIntegerBlend |
        kNativeShaderGpuReadback |
        kNativeShaderSameDeviceContext;
    if (kind == NativeGpuApiKind::ReferenceCpu) {
        result.flags |= kNativeShaderSoftwareDevice;
    }
    result.threadgroup_width = 8U;
    result.threadgroup_height = 8U;
    result.maximum_atlas_layers = 16U;
    result.shader_source_checksum = native_shader_source_checksum(kind);
    return result;
}

NativeShaderBindingLayout native_shader_binding_layout(
    NativeGpuApiKind kind) noexcept {
    NativeShaderBindingLayout result;
    if (kind == NativeGpuApiKind::Vulkan) {
        result.constants_binding = 0U;
        result.commands_binding = 1U;
        result.fills_binding = 2U;
        result.glyphs_binding = 3U;
        result.scissors_binding = 4U;
        result.atlas_metadata_binding = 5U;
        result.atlas_texture_binding = 5U;
        result.output_binding = 6U;
    } else if (kind == NativeGpuApiKind::Metal) {
        result.constants_binding = 0U;
        result.commands_binding = 1U;
        result.fills_binding = 2U;
        result.glyphs_binding = 3U;
        result.scissors_binding = 4U;
        result.atlas_metadata_binding = 0U;
        result.atlas_texture_binding = 0U;
        result.output_binding = 5U;
    }
    return result;
}

bool compile_native_shader_dispatch_plan(
    NativeGpuApiKind kind,
    const GpuShaderPacket& packet,
    const ShaderAtlasResidency& atlas,
    const NativeShaderExecutionLimits& limits,
    NativeShaderDispatchPlan* output,
    NativeShaderExecutionError* error) noexcept {
    clear_error(error);
    if (output == nullptr || packet.header.frame_id == 0U ||
        packet.header.surface_width == 0U ||
        packet.header.surface_height == 0U ||
        packet.header.packet_checksum != shader_packet_checksum(packet) ||
        (kind != NativeGpuApiKind::ReferenceCpu &&
         kind != NativeGpuApiKind::Direct3D12 &&
         kind != NativeGpuApiKind::Vulkan &&
         kind != NativeGpuApiKind::Metal)) {
        return fail(error, NativeShaderExecutionErrorKind::InvalidPacket,
                    "invalid native shader dispatch packet");
    }
    if (packet.header.command_count != packet.commands.size() ||
        packet.header.fill_instance_count != packet.fills.size() ||
        packet.header.glyph_instance_count != packet.glyphs.size() ||
        packet.header.scissor_count != packet.scissors.size() ||
        packet.header.packet_bytes > limits.maximum_packet_bytes ||
        packet.commands.size() > limits.maximum_commands ||
        packet.scissors.size() > limits.maximum_scissors ||
        packet.fills.size() > limits.maximum_fill_instances ||
        packet.glyphs.size() > limits.maximum_glyph_instances ||
        packet.header.surface_width > limits.maximum_surface_width ||
        packet.header.surface_height > limits.maximum_surface_height) {
        return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                    "shader packet exceeds native execution limits");
    }

    try {
        NativeShaderDispatchPlan candidate(output->atlas_bindings.get_allocator().resource());
        candidate.bindings = native_shader_binding_layout(kind);
        candidate.constants.surface_width = packet.header.surface_width;
        candidate.constants.surface_height = packet.header.surface_height;
        candidate.constants.command_count = packet.header.command_count;
        candidate.constants.frame_id = packet.header.frame_id;
        candidate.constants.packet_checksum = packet.header.packet_checksum;
        candidate.commands.reserve(packet.commands.size());
        candidate.fills.reserve(packet.fills.size());
        candidate.glyphs.reserve(packet.glyphs.size());
        candidate.scissors.reserve(packet.scissors.size());
        for (const GpuShaderDrawCommand& command : packet.commands) {
            NativeShaderDrawRecord record;
            record.kind = static_cast<std::uint32_t>(command.kind);
            record.layer = static_cast<std::uint32_t>(command.layer);
            record.atlas_format = static_cast<std::uint32_t>(command.atlas_format);
            record.scissor_index = command.scissor_index;
            record.first_instance = command.first_instance;
            record.instance_count = command.instance_count;
            record.atlas_page_index = command.atlas_page_index;
            record.stable_id = command.stable_id;
            candidate.commands.push_back(record);
        }
        for (const GpuShaderFillInstance& fill : packet.fills) {
            NativeShaderFillRecord record;
            record.destination = fill.destination;
            record.packed_color =
                static_cast<std::uint32_t>(fill.color.blue) |
                (static_cast<std::uint32_t>(fill.color.green) << 8U) |
                (static_cast<std::uint32_t>(fill.color.red) << 16U) |
                (static_cast<std::uint32_t>(fill.color.alpha) << 24U);
            record.scissor_index = fill.scissor_index;
            record.stable_id = fill.stable_id;
            candidate.fills.push_back(record);
        }
        for (const GpuShaderGlyphInstance& glyph : packet.glyphs) {
            NativeShaderGlyphRecord record;
            record.destination = glyph.destination;
            record.scissor_index = glyph.scissor_index;
            record.atlas_page_index = glyph.atlas_page_index;
            record.atlas_page_generation = glyph.atlas_page_generation;
            record.atlas_xy = static_cast<std::uint32_t>(glyph.atlas_x) |
                (static_cast<std::uint32_t>(glyph.atlas_y) << 16U);
            record.atlas_wh = static_cast<std::uint32_t>(glyph.atlas_width) |
                (static_cast<std::uint32_t>(glyph.atlas_height) << 16U);
            record.packed_color =
                static_cast<std::uint32_t>(glyph.color.blue) |
                (static_cast<std::uint32_t>(glyph.color.green) << 8U) |
                (static_cast<std::uint32_t>(glyph.color.red) << 16U) |
                (static_cast<std::uint32_t>(glyph.color.alpha) << 24U);
            record.format = static_cast<std::uint32_t>(glyph.format);
            record.stable_id = glyph.stable_id;
            candidate.glyphs.push_back(record);
        }
        for (const GpuShaderScissor& scissor : packet.scissors) {
            candidate.scissors.push_back(NativeShaderScissorRecord{scissor.rect});
        }

        std::uint32_t maximum_page_index = 0U;
        for (const GpuShaderGlyphInstance& glyph : packet.glyphs) {
            maximum_page_index = std::max(maximum_page_index, glyph.atlas_page_index);
            const ShaderAtlasResidentPage* page = atlas.find(
                glyph.atlas_page_index, glyph.atlas_page_generation);
            if (page == nullptr) {
                return fail(error, NativeShaderExecutionErrorKind::InvalidAtlasReference,
                            "shader glyph references a non-resident atlas page");
            }
            const auto existing = std::find_if(
                candidate.atlas_bindings.begin(), candidate.atlas_bindings.end(),
                [&](const NativeShaderAtlasBinding& binding) {
                    return binding.page_index == page->page_index &&
                        binding.page_generation == page->page_generation;
                });
            if (existing == candidate.atlas_bindings.end()) {
                if (candidate.atlas_bindings.size() >= limits.maximum_atlas_pages) {
                    return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                                "native shader atlas page limit exceeded");
                }
                NativeShaderAtlasBinding binding;
                binding.page_index = page->page_index;
                binding.page_generation = page->page_generation;
                binding.texture_layer = page->page_index;
                binding.width = page->width;
                binding.height = page->height;
                binding.row_bytes = static_cast<std::uint32_t>(page->width) * 4U;
                binding.resident_bytes = page->canonical_bgra.size();
                binding.content_checksum = shader_bytes_checksum(page->canonical_bgra);
                candidate.atlas_bindings.push_back(binding);
            }
        }
        if (!candidate.atlas_bindings.empty() &&
            maximum_page_index >= limits.maximum_atlas_pages) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "native shader atlas page index exceeds texture-array limit");
        }
        std::sort(
            candidate.atlas_bindings.begin(), candidate.atlas_bindings.end(),
            [](const NativeShaderAtlasBinding& left,
               const NativeShaderAtlasBinding& right) {
                return left.texture_layer < right.texture_layer;
            });
        candidate.constants.atlas_layer_count =
            static_cast<std::uint32_t>(candidate.atlas_bindings.size());

        auto span_bytes = [](std::size_t count, std::size_t item_size,
                             std::uint64_t* bytes) noexcept {
            return multiply_u64(count, item_size, bytes);
        };
        if (!span_bytes(candidate.commands.size(), sizeof(NativeShaderDrawRecord),
                        &candidate.header.command_bytes) ||
            !span_bytes(candidate.fills.size(), sizeof(NativeShaderFillRecord),
                        &candidate.header.fill_bytes) ||
            !span_bytes(candidate.glyphs.size(), sizeof(NativeShaderGlyphRecord),
                        &candidate.header.glyph_bytes) ||
            !span_bytes(candidate.scissors.size(), sizeof(NativeShaderScissorRecord),
                        &candidate.header.scissor_bytes) ||
            !multiply_u64(packet.header.surface_width,
                          packet.header.surface_height,
                          &candidate.header.output_bytes) ||
            !multiply_u64(candidate.header.output_bytes, 4U,
                          &candidate.header.output_bytes)) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "native shader dispatch byte count overflow");
        }
        candidate.header.atlas_bytes = 0U;
        candidate.header.atlas_checksum = kFnvOffset;
        for (const NativeShaderAtlasBinding& binding : candidate.atlas_bindings) {
            if (!add_u64(candidate.header.atlas_bytes,
                         binding.resident_bytes,
                         &candidate.header.atlas_bytes)) {
                return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                            "native shader atlas byte count overflow");
            }
            hash_value(&candidate.header.atlas_checksum, binding);
        }
        std::uint64_t staging_bytes = 0U;
        if (!add_u64(staging_bytes, candidate.header.command_bytes, &staging_bytes) ||
            !add_u64(staging_bytes, candidate.header.fill_bytes, &staging_bytes) ||
            !add_u64(staging_bytes, candidate.header.glyph_bytes, &staging_bytes) ||
            !add_u64(staging_bytes, candidate.header.scissor_bytes, &staging_bytes)) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "native shader staging byte count overflow");
        }
        if (candidate.header.atlas_bytes > limits.maximum_atlas_bytes ||
            candidate.header.output_bytes > limits.maximum_output_bytes ||
            staging_bytes > limits.maximum_staging_bytes) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "native shader dispatch exceeds memory limits");
        }

        candidate.header.api_kind = kind;
        candidate.header.flags = kNativeShaderExecutionReadback;
        const NativeShaderCapabilities capabilities =
            default_native_shader_capabilities(kind);
        candidate.header.dispatch_x =
            (packet.header.surface_width + capabilities.threadgroup_width - 1U) /
            capabilities.threadgroup_width;
        candidate.header.dispatch_y =
            (packet.header.surface_height + capabilities.threadgroup_height - 1U) /
            capabilities.threadgroup_height;
        candidate.header.dispatch_z = 1U;
        candidate.header.atlas_binding_count =
            static_cast<std::uint32_t>(candidate.atlas_bindings.size());
        candidate.header.packet_checksum = packet.header.packet_checksum;
        candidate.header.plan_checksum = native_shader_dispatch_plan_checksum(candidate);

        output->header = candidate.header;
        output->bindings = candidate.bindings;
        output->constants = candidate.constants;
        output->commands.swap(candidate.commands);
        output->fills.swap(candidate.fills);
        output->glyphs.swap(candidate.glyphs);
        output->scissors.swap(candidate.scissors);
        output->atlas_bindings.swap(candidate.atlas_bindings);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                    "native shader dispatch plan allocation failed");
    } catch (...) {
        return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                    "unexpected native shader dispatch planning failure");
    }
}

std::uint64_t native_shader_dispatch_plan_checksum(
    const NativeShaderDispatchPlan& plan) noexcept {
    NativeShaderDispatchPlanHeader header = plan.header;
    header.plan_checksum = 0U;
    std::uint64_t hash = kFnvOffset;
    hash_value(&hash, header);
    hash_value(&hash, plan.bindings);
    hash_value(&hash, plan.constants);
    for (const NativeShaderDrawRecord& record : plan.commands) hash_value(&hash, record);
    for (const NativeShaderFillRecord& record : plan.fills) hash_value(&hash, record);
    for (const NativeShaderGlyphRecord& record : plan.glyphs) hash_value(&hash, record);
    for (const NativeShaderScissorRecord& record : plan.scissors) hash_value(&hash, record);
    for (const NativeShaderAtlasBinding& binding : plan.atlas_bindings) {
        hash_value(&hash, binding);
    }
    return hash;
}

std::unique_ptr<NativeShaderExecutionApi>
make_reference_native_shader_execution_api() noexcept {
    try {
        return std::make_unique<ReferenceNativeShaderExecutionApi>();
    } catch (...) {
        return nullptr;
    }
}

#if !defined(ZEVRYON_HAS_D3D12_NATIVE_SHADER)
std::unique_ptr<NativeShaderExecutionApi>
make_direct3d12_native_shader_execution_api() noexcept {
    return nullptr;
}
#endif

#if !defined(ZEVRYON_HAS_VULKAN_NATIVE_SHADER)
std::unique_ptr<NativeShaderExecutionApi>
make_vulkan_native_shader_execution_api() noexcept {
    return nullptr;
}
#endif

#if !defined(ZEVRYON_HAS_METAL_NATIVE_SHADER)
std::unique_ptr<NativeShaderExecutionApi>
make_metal_native_shader_execution_api() noexcept {
    return nullptr;
}
#endif

std::string_view native_shader_hlsl_source() noexcept { return kHlslSource; }
std::string_view native_shader_glsl_source() noexcept { return kGlslSource; }
std::string_view native_shader_msl_source() noexcept { return kMslSource; }

std::uint64_t native_shader_source_checksum(NativeGpuApiKind kind) noexcept {
    std::uint64_t hash = kFnvOffset;
    if (kind == NativeGpuApiKind::Direct3D12) {
        hash_text(&hash, kHlslSource);
    } else if (kind == NativeGpuApiKind::Vulkan) {
        hash_text(&hash, kGlslSource);
    } else if (kind == NativeGpuApiKind::Metal) {
        hash_text(&hash, kMslSource);
    } else {
        hash_text(&hash, "reference-native-shader-executor-v1");
    }
    return hash;
}

} // namespace zevryon::text
