#include "native_shader_execution.hpp"

#if defined(ZEVRYON_HAS_D3D12_SHADER_EXECUTION)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

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

using Microsoft::WRL::ComPtr;
constexpr DWORD kFenceTimeoutMs = 10'000U;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct D3dCommand final {
    std::uint32_t kind{0U};
    std::uint32_t first_instance{0U};
    std::uint32_t instance_count{0U};
    std::uint32_t atlas_format{0U};
    std::int32_t scissor_x{0};
    std::int32_t scissor_y{0};
    std::int32_t scissor_width{0};
    std::int32_t scissor_height{0};
};
static_assert(sizeof(D3dCommand) == 32U);

struct D3dFill final {
    std::int32_t x{0};
    std::int32_t y{0};
    std::int32_t width{0};
    std::int32_t height{0};
    std::uint32_t color{0U};
    std::uint32_t reserved[3]{0U, 0U, 0U};
};
static_assert(sizeof(D3dFill) == 32U);

struct D3dGlyph final {
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
static_assert(sizeof(D3dGlyph) == 64U);

struct AtlasPageView final {
    const ShaderAtlasResidentPage* page{nullptr};
    std::uint32_t slice{0U};
};

constexpr const char* kIntegerComposerHlsl = R"HLSL(
struct CommandRecord {
    uint kind;
    uint firstInstance;
    uint instanceCount;
    uint atlasFormat;
    int scissorX;
    int scissorY;
    int scissorWidth;
    int scissorHeight;
};
struct FillRecord {
    int x;
    int y;
    int width;
    int height;
    uint color;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};
struct GlyphRecord {
    int x;
    int y;
    int width;
    int height;
    uint atlasSlice;
    uint atlasX;
    uint atlasY;
    uint atlasWidth;
    uint atlasHeight;
    uint color;
    uint format;
    uint reserved0;
    uint reserved1;
    uint reserved2;
    uint reserved3;
    uint reserved4;
};
StructuredBuffer<CommandRecord> Commands : register(t0);
StructuredBuffer<FillRecord> Fills : register(t1);
StructuredBuffer<GlyphRecord> Glyphs : register(t2);
Texture2DArray<uint> Atlas : register(t3);
RWTexture2D<uint> Output : register(u0);
cbuffer FrameConstants : register(b0) {
    uint SurfaceWidth;
    uint SurfaceHeight;
    uint CommandCount;
    uint Reserved;
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
uint blend_channel(uint source, uint destination, uint sourceAlpha) {
    uint inverse = 255U - sourceAlpha;
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
bool inside_rect(int2 pixel, int x, int y, int width, int height) {
    return pixel.x >= x && pixel.y >= y &&
           pixel.x < x + width && pixel.y < y + height;
}
uint premultiply_coverage(uint color, uint coverage) {
    uint alpha = mul_u8(channel_a(color), coverage);
    return pack_bgra(
        mul_u8(channel_b(color), alpha),
        mul_u8(channel_g(color), alpha),
        mul_u8(channel_r(color), alpha),
        alpha);
}
[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    if (dispatchId.x >= SurfaceWidth || dispatchId.y >= SurfaceHeight) {
        return;
    }
    int2 pixel = int2(dispatchId.xy);
    uint composed = 0U;
    for (uint commandIndex = 0U; commandIndex < CommandCount; ++commandIndex) {
        CommandRecord command = Commands[commandIndex];
        if (!inside_rect(pixel, command.scissorX, command.scissorY,
                         command.scissorWidth, command.scissorHeight)) {
            continue;
        }
        if (command.kind == 0U) {
            for (uint offset = 0U; offset < command.instanceCount; ++offset) {
                FillRecord fill = Fills[command.firstInstance + offset];
                if (inside_rect(pixel, fill.x, fill.y, fill.width, fill.height)) {
                    composed = blend_pixel(composed, fill.color);
                }
            }
        } else {
            for (uint offset = 0U; offset < command.instanceCount; ++offset) {
                GlyphRecord glyph = Glyphs[command.firstInstance + offset];
                if (!inside_rect(pixel, glyph.x, glyph.y, glyph.width, glyph.height)) {
                    continue;
                }
                uint localX = uint(pixel.x - glyph.x);
                uint localY = uint(pixel.y - glyph.y);
                uint sourceX = glyph.atlasX +
                    (localX * glyph.atlasWidth) / uint(glyph.width);
                uint sourceY = glyph.atlasY +
                    (localY * glyph.atlasHeight) / uint(glyph.height);
                uint texel = Atlas.Load(int4(sourceX, sourceY, glyph.atlasSlice, 0));
                uint source = 0U;
                if (glyph.format == 0U) {
                    source = premultiply_coverage(glyph.color, channel_a(texel));
                } else if (glyph.format == 1U) {
                    uint coverageB = channel_b(texel);
                    uint coverageG = channel_g(texel);
                    uint coverageR = channel_r(texel);
                    uint modulationAlpha = channel_a(glyph.color);
                    uint alpha = mul_u8(
                        modulationAlpha,
                        max(coverageB, max(coverageG, coverageR)));
                    source = pack_bgra(
                        mul_u8(mul_u8(channel_b(glyph.color), modulationAlpha), coverageB),
                        mul_u8(mul_u8(channel_g(glyph.color), modulationAlpha), coverageG),
                        mul_u8(mul_u8(channel_r(glyph.color), modulationAlpha), coverageR),
                        alpha);
                } else {
                    uint modulationAlpha = channel_a(glyph.color);
                    source = pack_bgra(
                        mul_u8(channel_b(texel), modulationAlpha),
                        mul_u8(channel_g(texel), modulationAlpha),
                        mul_u8(channel_r(texel), modulationAlpha),
                        mul_u8(channel_a(texel), modulationAlpha));
                }
                composed = blend_pixel(composed, source);
            }
        }
    }
    Output[dispatchId.xy] = composed;
}
)HLSL";

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
    HRESULT code = S_OK) noexcept {
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

bool checked_multiply(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t* result) noexcept {
    if (result == nullptr ||
        (left != 0U && right > (std::numeric_limits<std::uint64_t>::max)() / left)) {
        return false;
    }
    *result = left * right;
    return true;
}

template <typename T>
ComPtr<T> retain_com_pointer(std::uint64_t value) noexcept {
    ComPtr<T> output;
    T* pointer = reinterpret_cast<T*>(static_cast<std::uintptr_t>(value));
    if (pointer != nullptr) {
        pointer->AddRef();
        output.Attach(pointer);
    }
    return output;
}

std::uint32_t pack_color(ShaderColorBgra8 color) noexcept {
    return static_cast<std::uint32_t>(color.blue) |
           (static_cast<std::uint32_t>(color.green) << 8U) |
           (static_cast<std::uint32_t>(color.red) << 16U) |
           (static_cast<std::uint32_t>(color.alpha) << 24U);
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

class Direct3D12ShaderExecutor final : public NativeShaderExecutor {
public:
    ~Direct3D12ShaderExecutor() override { shutdown(); }

    NativeGpuApiKind kind() const noexcept override {
        return NativeGpuApiKind::Direct3D12;
    }

    bool configure(
        const NativeShaderExecutionConfig& config,
        NativeShaderExecutionError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (config.context.api_kind != NativeGpuApiKind::Direct3D12 ||
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
                        "invalid Direct3D 12 shader executor configuration");
        }
        shutdown_locked();
        device_ = retain_com_pointer<ID3D12Device>(config.context.device);
        queue_ = retain_com_pointer<ID3D12CommandQueue>(config.context.graphics_queue);
        if (device_ == nullptr || queue_ == nullptr) {
            shutdown_locked();
            return fail(error, NativeShaderExecutionErrorKind::NativeContextUnavailable,
                        "Direct3D 12 device or queue context is unavailable");
        }
        HRESULT result = device_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_));
        if (FAILED(result)) {
            shutdown_locked();
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "CreateCommandAllocator failed", result);
        }
        result = device_->CreateCommandList(
            0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
            IID_PPV_ARGS(&command_list_));
        if (FAILED(result)) {
            shutdown_locked();
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "CreateCommandList failed", result);
        }
        result = command_list_->Close();
        if (FAILED(result)) {
            shutdown_locked();
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "initial command-list close failed", result);
        }
        result = device_->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        if (FAILED(result)) {
            shutdown_locked();
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "CreateFence failed", result);
        }
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence_event_ == nullptr) {
            const HRESULT event_error = HRESULT_FROM_WIN32(GetLastError());
            shutdown_locked();
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "CreateEventW failed", event_error);
        }
        if (!create_pipeline_locked(error)) {
            shutdown_locked();
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 5U;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        result = device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&descriptor_heap_));
        if (FAILED(result)) {
            shutdown_locked();
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "CreateDescriptorHeap failed", result);
        }
        descriptor_increment_ = device_->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        config_ = config;
        snapshot_ = {};
        snapshot_.api_kind = NativeGpuApiKind::Direct3D12;
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
        if (snapshot_.configured == 0U || readback == nullptr ||
            packet.header.frame_id == 0U ||
            packet.header.packet_checksum != shader_packet_checksum(packet) ||
            packet.header.command_count != packet.commands.size() ||
            packet.header.fill_instance_count != packet.fills.size() ||
            packet.header.glyph_instance_count != packet.glyphs.size() ||
            packet.header.surface_width == 0U || packet.header.surface_height == 0U) {
            snapshot_.rejected_packets += 1U;
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "invalid or mutated shader packet");
        }
        if (packet.commands.size() > config_.limits.maximum_commands ||
            packet.fills.size() > config_.limits.maximum_fill_instances ||
            packet.glyphs.size() > config_.limits.maximum_glyph_instances ||
            packet.header.packet_bytes > config_.limits.maximum_packet_bytes ||
            packet.header.surface_width > config_.limits.maximum_surface_width ||
            packet.header.surface_height > config_.limits.maximum_surface_height) {
            snapshot_.rejected_packets += 1U;
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "shader packet exceeds Direct3D 12 execution limits");
        }
        try {
            std::vector<AtlasPageView> pages;
            if (!collect_pages_locked(packet, atlas, &pages, error) ||
                !ensure_atlas_locked(pages, error) ||
                !ensure_output_locked(
                    packet.header.surface_width,
                    packet.header.surface_height,
                    error)) {
                return false;
            }

            std::vector<D3dCommand> commands;
            std::vector<D3dFill> fills;
            std::vector<D3dGlyph> glyphs;
            if (!pack_records_locked(packet, pages, &commands, &fills, &glyphs, error)) {
                return false;
            }

            ComPtr<ID3D12Resource> command_buffer;
            ComPtr<ID3D12Resource> fill_buffer;
            ComPtr<ID3D12Resource> glyph_buffer;
            if (!create_upload_buffer_locked(
                    std::as_bytes(std::span<const D3dCommand>(commands)),
                    &command_buffer, error) ||
                !create_upload_buffer_locked(
                    std::as_bytes(std::span<const D3dFill>(fills)),
                    &fill_buffer, error) ||
                !create_upload_buffer_locked(
                    std::as_bytes(std::span<const D3dGlyph>(glyphs)),
                    &glyph_buffer, error)) {
                return false;
            }
            create_descriptors_locked(
                command_buffer.Get(), static_cast<std::uint32_t>(commands.size()),
                fill_buffer.Get(), static_cast<std::uint32_t>(fills.size()),
                glyph_buffer.Get(), static_cast<std::uint32_t>(glyphs.size()));

            HRESULT result = allocator_->Reset();
            if (FAILED(result)) {
                return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                            "command allocator reset failed", result);
            }
            result = command_list_->Reset(allocator_.Get(), pipeline_state_.Get());
            if (FAILED(result)) {
                return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                            "command list reset failed", result);
            }
            if (output_state_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = output_texture_.Get();
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = output_state_;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                command_list_->ResourceBarrier(1U, &barrier);
                output_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
            ID3D12DescriptorHeap* heaps[] = {descriptor_heap_.Get()};
            command_list_->SetDescriptorHeaps(1U, heaps);
            command_list_->SetComputeRootSignature(root_signature_.Get());
            const D3D12_GPU_DESCRIPTOR_HANDLE gpu_start =
                descriptor_heap_->GetGPUDescriptorHandleForHeapStart();
            command_list_->SetComputeRootDescriptorTable(0U, gpu_start);
            D3D12_GPU_DESCRIPTOR_HANDLE output_handle = gpu_start;
            output_handle.ptr += static_cast<UINT64>(descriptor_increment_) * 4U;
            command_list_->SetComputeRootDescriptorTable(1U, output_handle);
            const std::array<std::uint32_t, 4U> constants{
                packet.header.surface_width,
                packet.header.surface_height,
                static_cast<std::uint32_t>(packet.commands.size()),
                0U};
            command_list_->SetComputeRoot32BitConstants(
                2U, static_cast<UINT>(constants.size()), constants.data(), 0U);
            command_list_->Dispatch(
                (packet.header.surface_width + 7U) / 8U,
                (packet.header.surface_height + 7U) / 8U,
                1U);
            D3D12_RESOURCE_BARRIER uav{};
            uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav.UAV.pResource = output_texture_.Get();
            command_list_->ResourceBarrier(1U, &uav);
            D3D12_RESOURCE_BARRIER to_copy{};
            to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            to_copy.Transition.pResource = output_texture_.Get();
            to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            command_list_->ResourceBarrier(1U, &to_copy);
            output_state_ = D3D12_RESOURCE_STATE_COPY_SOURCE;

            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = output_texture_.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = 0U;
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = readback_buffer_.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = readback_footprint_;
            command_list_->CopyTextureRegion(&destination, 0U, 0U, 0U, &source, nullptr);
            result = command_list_->Close();
            if (FAILED(result)) {
                return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                            "command list close failed", result);
            }
            ID3D12CommandList* lists[] = {command_list_.Get()};
            queue_->ExecuteCommandLists(1U, lists);
            if (!signal_and_wait_locked(error)) {
                return false;
            }

            ShaderReadback candidate{};
            candidate.width = packet.header.surface_width;
            candidate.height = packet.header.surface_height;
            candidate.row_bytes = packet.header.surface_width * 4U;
            std::uint64_t surface_bytes = 0U;
            if (!checked_multiply(candidate.row_bytes, candidate.height, &surface_bytes) ||
                surface_bytes > config_.limits.maximum_readback_bytes ||
                surface_bytes > (std::numeric_limits<std::size_t>::max)()) {
                return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                            "readback byte count exceeds the configured limit");
            }
            candidate.bgra.resize(static_cast<std::size_t>(surface_bytes));
            void* mapped = nullptr;
            D3D12_RANGE read_range{0U, static_cast<SIZE_T>(readback_total_bytes_)};
            result = readback_buffer_->Map(0U, &read_range, &mapped);
            if (FAILED(result) || mapped == nullptr) {
                return fail(error, NativeShaderExecutionErrorKind::ReadbackFailed,
                            "readback buffer mapping failed", result);
            }
            const auto* source_bytes = static_cast<const std::byte*>(mapped) +
                static_cast<std::size_t>(readback_footprint_.Offset);
            for (std::uint32_t row = 0U; row < candidate.height; ++row) {
                std::memcpy(
                    candidate.bgra.data() + static_cast<std::size_t>(row) * candidate.row_bytes,
                    source_bytes + static_cast<std::size_t>(row) *
                        readback_footprint_.Footprint.RowPitch,
                    candidate.row_bytes);
            }
            D3D12_RANGE write_range{0U, 0U};
            readback_buffer_->Unmap(0U, &write_range);
            candidate.checksum = shader_bytes_checksum(candidate.bgra);
            *readback = std::move(candidate);

            snapshot_.executions += 1U;
            snapshot_.readbacks += 1U;
            snapshot_.last_packet_checksum = packet.header.packet_checksum;
            snapshot_.last_readback_checksum = readback->checksum;
            const std::uint64_t transient =
                static_cast<std::uint64_t>(commands.size()) * sizeof(D3dCommand) +
                static_cast<std::uint64_t>(fills.size()) * sizeof(D3dFill) +
                static_cast<std::uint64_t>(glyphs.size()) * sizeof(D3dGlyph) +
                readback_total_bytes_;
            snapshot_.peak_transient_bytes = std::max(
                snapshot_.peak_transient_bytes, transient);
            return true;
        } catch (const std::bad_alloc&) {
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "Direct3D 12 shader execution allocation failed");
        } catch (...) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "unexpected Direct3D 12 shader execution failure");
        }
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
    bool create_pipeline_locked(NativeShaderExecutionError* error) noexcept {
        ComPtr<ID3DBlob> shader;
        ComPtr<ID3DBlob> diagnostics;
        HRESULT result = D3DCompile(
            kIntegerComposerHlsl,
            std::strlen(kIntegerComposerHlsl),
            "zevryon_integer_composer.hlsl",
            nullptr, nullptr, "main", "cs_5_1",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0U, &shader, &diagnostics);
        if (FAILED(result)) {
            if (error != nullptr && diagnostics != nullptr) {
                error->kind = NativeShaderExecutionErrorKind::ShaderCompilationFailed;
                error->native_code = static_cast<std::int64_t>(result);
                try {
                    error->message.assign(
                        static_cast<const char*>(diagnostics->GetBufferPointer()),
                        diagnostics->GetBufferSize());
                } catch (...) {
                    error->message.clear();
                }
                return false;
            }
            return fail(error, NativeShaderExecutionErrorKind::ShaderCompilationFailed,
                        "D3DCompile failed", result);
        }

        std::array<D3D12_DESCRIPTOR_RANGE, 2U> ranges{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 4U;
        ranges[0].BaseShaderRegister = 0U;
        ranges[0].RegisterSpace = 0U;
        ranges[0].OffsetInDescriptorsFromTableStart = 0U;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1U;
        ranges[1].BaseShaderRegister = 0U;
        ranges[1].RegisterSpace = 0U;
        ranges[1].OffsetInDescriptorsFromTableStart = 0U;
        std::array<D3D12_ROOT_PARAMETER, 3U> parameters{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable.NumDescriptorRanges = 1U;
        parameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1U;
        parameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants.ShaderRegister = 0U;
        parameters[2].Constants.RegisterSpace = 0U;
        parameters[2].Constants.Num32BitValues = 4U;
        parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC signature_desc{};
        signature_desc.NumParameters = static_cast<UINT>(parameters.size());
        signature_desc.pParameters = parameters.data();
        signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> signature_error;
        result = D3D12SerializeRootSignature(
            &signature_desc, D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized, &signature_error);
        if (FAILED(result)) {
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "D3D12SerializeRootSignature failed", result);
        }
        result = device_->CreateRootSignature(
            0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&root_signature_));
        if (FAILED(result)) {
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "CreateRootSignature failed", result);
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
        pipeline_desc.pRootSignature = root_signature_.Get();
        pipeline_desc.CS.pShaderBytecode = shader->GetBufferPointer();
        pipeline_desc.CS.BytecodeLength = shader->GetBufferSize();
        result = device_->CreateComputePipelineState(
            &pipeline_desc, IID_PPV_ARGS(&pipeline_state_));
        if (FAILED(result)) {
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "CreateComputePipelineState failed", result);
        }
        return true;
    }

    bool collect_pages_locked(
        const GpuShaderPacket& packet,
        const ShaderAtlasResidency& atlas,
        std::vector<AtlasPageView>* pages,
        NativeShaderExecutionError* error) noexcept {
        if (pages == nullptr) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "atlas page output is null");
        }
        pages->clear();
        for (const GpuShaderGlyphInstance& glyph : packet.glyphs) {
            const auto existing = std::find_if(
                pages->begin(), pages->end(),
                [&](const AtlasPageView& item) {
                    return item.page->page_index == glyph.atlas_page_index;
                });
            if (existing != pages->end()) {
                if (existing->page->page_generation != glyph.atlas_page_generation) {
                    return fail(error, NativeShaderExecutionErrorKind::StaleGeneration,
                                "packet references two generations of one atlas page");
                }
                continue;
            }
            const ShaderAtlasResidentPage* page = atlas.find(
                glyph.atlas_page_index, glyph.atlas_page_generation);
            if (page == nullptr) {
                return fail(error, NativeShaderExecutionErrorKind::StaleGeneration,
                            "packet references a non-resident atlas page");
            }
            std::uint64_t expected = 0U;
            if (!checked_multiply(page->width, page->height, &expected) ||
                !checked_multiply(expected, 4U, &expected) ||
                expected != page->canonical_bgra.size()) {
                return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                            "resident atlas page has invalid canonical byte size");
            }
            pages->push_back(AtlasPageView{page, 0U});
        }
        std::sort(
            pages->begin(), pages->end(),
            [](const AtlasPageView& left, const AtlasPageView& right) {
                return left.page->page_index < right.page->page_index;
            });
        if (pages->size() > config_.limits.maximum_atlas_pages) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "packet references too many atlas pages");
        }
        for (std::size_t index = 0U; index < pages->size(); ++index) {
            (*pages)[index].slice = static_cast<std::uint32_t>(index);
        }
        return true;
    }

    bool ensure_atlas_locked(
        const std::vector<AtlasPageView>& pages,
        NativeShaderExecutionError* error) noexcept {
        std::uint64_t signature = kFnvOffset;
        std::uint64_t atlas_bytes = 0U;
        std::uint32_t width = 1U;
        std::uint32_t height = 1U;
        for (const AtlasPageView& item : pages) {
            hash_value(&signature, item.page->page_index);
            hash_value(&signature, item.page->page_generation);
            hash_value(&signature, item.page->width);
            hash_value(&signature, item.page->height);
            const std::uint64_t checksum = shader_bytes_checksum(item.page->canonical_bgra);
            hash_value(&signature, checksum);
            atlas_bytes += item.page->canonical_bgra.size();
            width = std::max(width, static_cast<std::uint32_t>(item.page->width));
            height = std::max(height, static_cast<std::uint32_t>(item.page->height));
        }
        if (atlas_bytes > config_.limits.maximum_atlas_bytes) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "persistent atlas exceeds configured byte limit");
        }
        if (atlas_texture_ != nullptr && signature == atlas_signature_) {
            snapshot_.atlas_reuses += 1U;
            return true;
        }

        const std::uint16_t slices = static_cast<std::uint16_t>(
            std::max<std::size_t>(1U, pages.size()));
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC texture_desc{};
        texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture_desc.Width = width;
        texture_desc.Height = height;
        texture_desc.DepthOrArraySize = slices;
        texture_desc.MipLevels = 1U;
        texture_desc.Format = DXGI_FORMAT_R32_UINT;
        texture_desc.SampleDesc.Count = 1U;
        texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texture_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> candidate_texture;
        HRESULT result = device_->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
            pages.empty() ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                          : D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&candidate_texture));
        if (FAILED(result)) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                        "atlas texture allocation failed", result);
        }
        if (!pages.empty()) {
            const UINT subresource_count = static_cast<UINT>(pages.size());
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresource_count);
            std::vector<UINT> row_counts(subresource_count);
            std::vector<UINT64> row_sizes(subresource_count);
            UINT64 upload_bytes = 0U;
            device_->GetCopyableFootprints(
                &texture_desc, 0U, subresource_count, 0U,
                layouts.data(), row_counts.data(), row_sizes.data(), &upload_bytes);
            D3D12_HEAP_PROPERTIES upload_heap{};
            upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC upload_desc{};
            upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            upload_desc.Width = upload_bytes;
            upload_desc.Height = 1U;
            upload_desc.DepthOrArraySize = 1U;
            upload_desc.MipLevels = 1U;
            upload_desc.SampleDesc.Count = 1U;
            upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> upload;
            result = device_->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&upload));
            if (FAILED(result)) {
                return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                            "atlas upload buffer allocation failed", result);
            }
            void* mapped = nullptr;
            result = upload->Map(0U, nullptr, &mapped);
            if (FAILED(result) || mapped == nullptr) {
                return fail(error, NativeShaderExecutionErrorKind::AtlasUploadFailed,
                            "atlas upload buffer mapping failed", result);
            }
            std::memset(mapped, 0, static_cast<std::size_t>(upload_bytes));
            for (UINT slice = 0U; slice < subresource_count; ++slice) {
                const ShaderAtlasResidentPage& page = *pages[slice].page;
                auto* destination = static_cast<std::byte*>(mapped) +
                    static_cast<std::size_t>(layouts[slice].Offset);
                const std::size_t source_row_bytes =
                    static_cast<std::size_t>(page.width) * 4U;
                for (std::uint32_t row = 0U; row < page.height; ++row) {
                    std::memcpy(
                        destination + static_cast<std::size_t>(row) *
                            layouts[slice].Footprint.RowPitch,
                        page.canonical_bgra.data() +
                            static_cast<std::size_t>(row) * source_row_bytes,
                        source_row_bytes);
                }
            }
            upload->Unmap(0U, nullptr);
            result = allocator_->Reset();
            if (FAILED(result)) {
                return fail(error, NativeShaderExecutionErrorKind::AtlasUploadFailed,
                            "allocator reset before atlas upload failed", result);
            }
            result = command_list_->Reset(allocator_.Get(), nullptr);
            if (FAILED(result)) {
                return fail(error, NativeShaderExecutionErrorKind::AtlasUploadFailed,
                            "command list reset before atlas upload failed", result);
            }
            for (UINT slice = 0U; slice < subresource_count; ++slice) {
                D3D12_TEXTURE_COPY_LOCATION source{};
                source.pResource = upload.Get();
                source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                source.PlacedFootprint = layouts[slice];
                D3D12_TEXTURE_COPY_LOCATION destination{};
                destination.pResource = candidate_texture.Get();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                destination.SubresourceIndex = slice;
                command_list_->CopyTextureRegion(
                    &destination, 0U, 0U, 0U, &source, nullptr);
            }
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = candidate_texture.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            command_list_->ResourceBarrier(1U, &barrier);
            result = command_list_->Close();
            if (FAILED(result)) {
                return fail(error, NativeShaderExecutionErrorKind::AtlasUploadFailed,
                            "atlas upload command list close failed", result);
            }
            ID3D12CommandList* lists[] = {command_list_.Get()};
            queue_->ExecuteCommandLists(1U, lists);
            if (!signal_and_wait_locked(error)) {
                return false;
            }
        }
        atlas_texture_ = std::move(candidate_texture);
        atlas_signature_ = signature;
        atlas_width_ = width;
        atlas_height_ = height;
        atlas_slices_ = slices;
        snapshot_.atlas_upload_batches += 1U;
        snapshot_.persistent_atlas_bytes = atlas_bytes;
        return true;
    }

    bool ensure_output_locked(
        std::uint32_t width,
        std::uint32_t height,
        NativeShaderExecutionError* error) noexcept {
        if (output_texture_ != nullptr && output_width_ == width &&
            output_height_ == height) {
            return true;
        }
        std::uint64_t bytes = 0U;
        if (!checked_multiply(width, height, &bytes) ||
            !checked_multiply(bytes, 4U, &bytes) ||
            bytes > config_.limits.maximum_readback_bytes) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceBudgetExceeded,
                        "output surface exceeds configured byte limit");
        }
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1U;
        desc.MipLevels = 1U;
        desc.Format = DXGI_FORMAT_R32_UINT;
        desc.SampleDesc.Count = 1U;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> output;
        HRESULT result = device_->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&output));
        if (FAILED(result)) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                        "output UAV texture allocation failed", result);
        }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0U;
        UINT64 row_size = 0U;
        UINT64 total = 0U;
        device_->GetCopyableFootprints(
            &desc, 0U, 1U, 0U, &footprint, &rows, &row_size, &total);
        (void)rows;
        (void)row_size;
        D3D12_HEAP_PROPERTIES readback_heap{};
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC readback_desc{};
        readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readback_desc.Width = total;
        readback_desc.Height = 1U;
        readback_desc.DepthOrArraySize = 1U;
        readback_desc.MipLevels = 1U;
        readback_desc.SampleDesc.Count = 1U;
        readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        result = device_->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback));
        if (FAILED(result)) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                        "readback buffer allocation failed", result);
        }
        output_texture_ = std::move(output);
        readback_buffer_ = std::move(readback);
        readback_footprint_ = footprint;
        readback_total_bytes_ = total;
        output_width_ = width;
        output_height_ = height;
        output_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        snapshot_.output_surface_bytes = bytes;
        return true;
    }

    bool pack_records_locked(
        const GpuShaderPacket& packet,
        const std::vector<AtlasPageView>& pages,
        std::vector<D3dCommand>* commands,
        std::vector<D3dFill>* fills,
        std::vector<D3dGlyph>* glyphs,
        NativeShaderExecutionError* error) noexcept {
        if (commands == nullptr || fills == nullptr || glyphs == nullptr) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "packed record output is null");
        }
        commands->reserve(packet.commands.size());
        fills->reserve(std::max<std::size_t>(1U, packet.fills.size()));
        glyphs->reserve(std::max<std::size_t>(1U, packet.glyphs.size()));
        for (const GpuShaderFillInstance& fill : packet.fills) {
            fills->push_back(D3dFill{
                fill.destination.x, fill.destination.y,
                fill.destination.width, fill.destination.height,
                pack_color(fill.color), {0U, 0U, 0U}});
        }
        if (fills->empty()) {
            fills->push_back({});
        }
        for (const GpuShaderGlyphInstance& glyph : packet.glyphs) {
            const auto page = std::find_if(
                pages.begin(), pages.end(),
                [&](const AtlasPageView& candidate) {
                    return candidate.page->page_index == glyph.atlas_page_index &&
                           candidate.page->page_generation == glyph.atlas_page_generation;
                });
            if (page == pages.end()) {
                return fail(error, NativeShaderExecutionErrorKind::StaleGeneration,
                            "glyph page generation is not resident");
            }
            glyphs->push_back(D3dGlyph{
                glyph.destination.x, glyph.destination.y,
                glyph.destination.width, glyph.destination.height,
                page->slice,
                glyph.atlas_x, glyph.atlas_y,
                glyph.atlas_width, glyph.atlas_height,
                pack_color(glyph.color),
                static_cast<std::uint32_t>(glyph.format),
                {0U, 0U, 0U, 0U, 0U}});
        }
        if (glyphs->empty()) {
            glyphs->push_back({});
        }
        for (const GpuShaderDrawCommand& command : packet.commands) {
            if (command.scissor_index >= packet.scissors.size()) {
                return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                            "draw command references invalid scissor");
            }
            const ShaderRectI rect = packet.scissors[command.scissor_index].rect;
            commands->push_back(D3dCommand{
                static_cast<std::uint32_t>(command.kind),
                command.first_instance,
                command.instance_count,
                static_cast<std::uint32_t>(command.atlas_format),
                rect.x, rect.y, rect.width, rect.height});
        }
        return true;
    }

    bool create_upload_buffer_locked(
        std::span<const std::byte> bytes,
        ComPtr<ID3D12Resource>* resource,
        NativeShaderExecutionError* error) noexcept {
        if (resource == nullptr) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "upload resource output is null");
        }
        const std::uint64_t size = std::max<std::uint64_t>(4U, bytes.size());
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1U;
        desc.DepthOrArraySize = 1U;
        desc.MipLevels = 1U;
        desc.SampleDesc.Count = 1U;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HRESULT result = device_->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(resource->ReleaseAndGetAddressOf()));
        if (FAILED(result)) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                        "structured upload buffer allocation failed", result);
        }
        void* mapped = nullptr;
        result = (*resource)->Map(0U, nullptr, &mapped);
        if (FAILED(result) || mapped == nullptr) {
            return fail(error, NativeShaderExecutionErrorKind::ResourceAllocationFailed,
                        "structured upload buffer mapping failed", result);
        }
        std::memset(mapped, 0, static_cast<std::size_t>(size));
        if (!bytes.empty()) {
            std::memcpy(mapped, bytes.data(), bytes.size());
        }
        (*resource)->Unmap(0U, nullptr);
        return true;
    }

    void create_descriptors_locked(
        ID3D12Resource* commands,
        std::uint32_t command_count,
        ID3D12Resource* fills,
        std::uint32_t fill_count,
        ID3D12Resource* glyphs,
        std::uint32_t glyph_count) noexcept {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
        const auto create_buffer_srv = [&](ID3D12Resource* resource,
                                           std::uint32_t count,
                                           std::uint32_t stride) mutable {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.Buffer.FirstElement = 0U;
            desc.Buffer.NumElements = std::max(1U, count);
            desc.Buffer.StructureByteStride = stride;
            desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
            device_->CreateShaderResourceView(resource, &desc, handle);
            handle.ptr += descriptor_increment_;
        };
        create_buffer_srv(commands, command_count, sizeof(D3dCommand));
        create_buffer_srv(fills, fill_count, sizeof(D3dFill));
        create_buffer_srv(glyphs, glyph_count, sizeof(D3dGlyph));
        D3D12_SHADER_RESOURCE_VIEW_DESC atlas_desc{};
        atlas_desc.Format = DXGI_FORMAT_R32_UINT;
        atlas_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        atlas_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        atlas_desc.Texture2DArray.MostDetailedMip = 0U;
        atlas_desc.Texture2DArray.MipLevels = 1U;
        atlas_desc.Texture2DArray.FirstArraySlice = 0U;
        atlas_desc.Texture2DArray.ArraySize = atlas_slices_;
        atlas_desc.Texture2DArray.PlaneSlice = 0U;
        atlas_desc.Texture2DArray.ResourceMinLODClamp = 0.0F;
        device_->CreateShaderResourceView(atlas_texture_.Get(), &atlas_desc, handle);
        handle.ptr += descriptor_increment_;
        D3D12_UNORDERED_ACCESS_VIEW_DESC output_desc{};
        output_desc.Format = DXGI_FORMAT_R32_UINT;
        output_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        output_desc.Texture2D.MipSlice = 0U;
        output_desc.Texture2D.PlaneSlice = 0U;
        device_->CreateUnorderedAccessView(
            output_texture_.Get(), nullptr, &output_desc, handle);
    }

    bool signal_and_wait_locked(NativeShaderExecutionError* error) noexcept {
        const std::uint64_t signal = next_fence_value_++;
        HRESULT result = queue_->Signal(fence_.Get(), signal);
        if (FAILED(result)) {
            return fail(error, NativeShaderExecutionErrorKind::DeviceLost,
                        "command queue signal failed", result);
        }
        if (fence_->GetCompletedValue() < signal) {
            result = fence_->SetEventOnCompletion(signal, fence_event_);
            if (FAILED(result)) {
                return fail(error, NativeShaderExecutionErrorKind::SubmissionFailed,
                            "fence event registration failed", result);
            }
            const DWORD wait = WaitForSingleObject(fence_event_, kFenceTimeoutMs);
            if (wait != WAIT_OBJECT_0) {
                return fail(error, NativeShaderExecutionErrorKind::FenceTimeout,
                            "GPU fence wait timed out",
                            HRESULT_FROM_WIN32(wait == WAIT_FAILED ? GetLastError() : WAIT_TIMEOUT));
            }
        }
        return true;
    }

    void shutdown_locked() noexcept {
        if (queue_ != nullptr && fence_ != nullptr && fence_event_ != nullptr) {
            NativeShaderExecutionError ignored;
            (void)signal_and_wait_locked(&ignored);
        }
        if (fence_event_ != nullptr) {
            CloseHandle(fence_event_);
            fence_event_ = nullptr;
        }
        readback_buffer_.Reset();
        output_texture_.Reset();
        atlas_texture_.Reset();
        descriptor_heap_.Reset();
        pipeline_state_.Reset();
        root_signature_.Reset();
        command_list_.Reset();
        allocator_.Reset();
        fence_.Reset();
        queue_.Reset();
        device_.Reset();
        config_ = {};
        snapshot_ = {};
        atlas_signature_ = 0U;
        atlas_width_ = 0U;
        atlas_height_ = 0U;
        atlas_slices_ = 0U;
        output_width_ = 0U;
        output_height_ = 0U;
        readback_total_bytes_ = 0U;
        output_state_ = D3D12_RESOURCE_STATE_COMMON;
        next_fence_value_ = 1U;
    }

    mutable std::mutex mutex_;
    NativeShaderExecutionConfig config_{};
    NativeShaderExecutionSnapshot snapshot_{};
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> command_list_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_{nullptr};
    std::uint64_t next_fence_value_{1U};
    ComPtr<ID3D12RootSignature> root_signature_;
    ComPtr<ID3D12PipelineState> pipeline_state_;
    ComPtr<ID3D12DescriptorHeap> descriptor_heap_;
    UINT descriptor_increment_{0U};
    ComPtr<ID3D12Resource> atlas_texture_;
    std::uint64_t atlas_signature_{0U};
    std::uint32_t atlas_width_{0U};
    std::uint32_t atlas_height_{0U};
    std::uint16_t atlas_slices_{0U};
    ComPtr<ID3D12Resource> output_texture_;
    ComPtr<ID3D12Resource> readback_buffer_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT readback_footprint_{};
    UINT64 readback_total_bytes_{0U};
    std::uint32_t output_width_{0U};
    std::uint32_t output_height_{0U};
    D3D12_RESOURCE_STATES output_state_{D3D12_RESOURCE_STATE_COMMON};
};

} // namespace

std::unique_ptr<NativeShaderExecutor>
make_direct3d12_native_shader_executor() noexcept {
    try {
        return std::make_unique<Direct3D12ShaderExecutor>();
    } catch (...) {
        return nullptr;
    }
}

} // namespace zevryon::text

#endif
