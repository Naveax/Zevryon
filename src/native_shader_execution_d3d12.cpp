#include "native_shader_execution.hpp"

#if defined(ZEVRYON_HAS_D3D12_NATIVE_SHADER)

#ifndef NOMINMAX
#define NOMINMAX
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
#include <utility>
#include <vector>

namespace zevryon::text {
namespace {

using Microsoft::WRL::ComPtr;
constexpr DWORD kFenceTimeoutMs = 5000U;
constexpr UINT kDescriptorCount = 7U;

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

D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) noexcept {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1U;
    properties.VisibleNodeMask = 1U;
    return properties;
}

D3D12_RESOURCE_DESC buffer_description(
    std::uint64_t bytes,
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) noexcept {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Alignment = 0U;
    description.Width = std::max<std::uint64_t>(bytes, 4U);
    description.Height = 1U;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1U;
    description.SampleDesc.Quality = 0U;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;
    return description;
}

class Direct3D12NativeShaderExecutionApi final
    : public NativeShaderExecutionApi {
public:
    ~Direct3D12NativeShaderExecutionApi() override { shutdown(); }

    NativeGpuApiKind kind() const noexcept override {
        return NativeGpuApiKind::Direct3D12;
    }

    NativeShaderCapabilities capabilities() const noexcept override {
        NativeShaderCapabilities result =
            default_native_shader_capabilities(NativeGpuApiKind::Direct3D12);
        if ((snapshot_.context.flags & kNativeGpuSdkContextSoftwareDevice) != 0U) {
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
        if (context.api_kind != NativeGpuApiKind::Direct3D12 ||
            context.device_generation == 0U || context.runtime_generation == 0U ||
            context.device == 0U || context.graphics_queue == 0U ||
            limits.maximum_atlas_pages == 0U ||
            limits.maximum_atlas_pages > atlas_generations_.size() ||
            limits.maximum_frames_in_flight == 0U ||
            limits.maximum_output_bytes == 0U) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "invalid Direct3D 12 shader execution configuration");
        }
        device_ = retain_com_pointer<ID3D12Device>(context.device);
        queue_ = retain_com_pointer<ID3D12CommandQueue>(context.graphics_queue);
        if (device_ == nullptr || queue_ == nullptr) {
            shutdown_locked();
            return fail(error, NativeShaderExecutionErrorKind::StaleGeneration,
                        "Direct3D 12 shader context pointers could not be retained");
        }

        HRESULT result = compile_pipeline_locked(error);
        if (FAILED(result)) {
            shutdown_locked();
            return false;
        }
        result = device_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_));
        if (FAILED(result)) {
            shutdown_locked();
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "Direct3D 12 shader command allocator creation failed", result);
        }
        result = device_->CreateCommandList(
            0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(),
            pipeline_.Get(), IID_PPV_ARGS(&command_list_));
        if (FAILED(result)) {
            shutdown_locked();
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "Direct3D 12 shader command list creation failed", result);
        }
        result = command_list_->Close();
        if (FAILED(result)) {
            shutdown_locked();
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "Direct3D 12 initial shader command list close failed", result);
        }
        result = device_->CreateFence(0U, D3D12_FENCE_FLAG_NONE,
                                      IID_PPV_ARGS(&fence_));
        if (FAILED(result)) {
            shutdown_locked();
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "Direct3D 12 shader fence creation failed", result);
        }
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence_event_ == nullptr) {
            const HRESULT code = HRESULT_FROM_WIN32(GetLastError());
            shutdown_locked();
            return fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                        "Direct3D 12 shader fence event creation failed", code);
        }

        limits_ = limits;
        snapshot_ = {};
        snapshot_.capabilities = default_native_shader_capabilities(
            NativeGpuApiKind::Direct3D12);
        if ((context.flags & kNativeGpuSdkContextSoftwareDevice) != 0U) {
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
        if (!configured_ || request.packet == nullptr || request.atlas == nullptr ||
            receipt == nullptr || request.ticket_id == 0U) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "invalid Direct3D 12 shader execution request");
        }
        NativeShaderDispatchPlan plan;
        if (!compile_native_shader_dispatch_plan(
                NativeGpuApiKind::Direct3D12, *request.packet, *request.atlas,
                limits_, &plan, error)) {
            return false;
        }
        if (!ensure_resources_locked(plan, error)) {
            return false;
        }

        HRESULT result = allocator_->Reset();
        if (FAILED(result)) {
            return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                        "Direct3D 12 shader allocator reset failed", result);
        }
        result = command_list_->Reset(allocator_.Get(), pipeline_.Get());
        if (FAILED(result)) {
            return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                        "Direct3D 12 shader command list reset failed", result);
        }
        std::vector<ComPtr<ID3D12Resource>> atlas_uploads;
        std::array<std::uint32_t, 16U> pending_generations = atlas_generations_;
        std::uint64_t pending_upload_count = 0U;
        const D3D12_RESOURCE_STATES atlas_state_before = atlas_state_;
        if (!upload_atlas_locked(
                plan, *request.atlas, &atlas_uploads,
                &pending_generations, &pending_upload_count, error)) {
            atlas_state_ = atlas_state_before;
            (void)command_list_->Close();
            return false;
        }
        if (!copy_upload_locked(
                commands_, plan.commands.data(), plan.header.command_bytes, error) ||
            !copy_upload_locked(
                fills_, plan.fills.data(), plan.header.fill_bytes, error) ||
            !copy_upload_locked(
                glyphs_, plan.glyphs.data(), plan.header.glyph_bytes, error) ||
            !copy_upload_locked(
                scissors_, plan.scissors.data(), plan.header.scissor_bytes, error)) {
            atlas_state_ = atlas_state_before;
            (void)command_list_->Close();
            return false;
        }

        create_descriptors_locked(plan);
        command_list_->SetComputeRootSignature(root_signature_.Get());
        ID3D12DescriptorHeap* heaps[] = {descriptor_heap_.Get()};
        command_list_->SetDescriptorHeaps(1U, heaps);
        const std::array<std::uint32_t, 8U> constants{
            plan.constants.surface_width,
            plan.constants.surface_height,
            plan.constants.command_count,
            plan.constants.atlas_layer_count,
            static_cast<std::uint32_t>(plan.constants.frame_id),
            static_cast<std::uint32_t>(plan.constants.frame_id >> 32U),
            static_cast<std::uint32_t>(plan.constants.packet_checksum),
            static_cast<std::uint32_t>(plan.constants.packet_checksum >> 32U)};
        command_list_->SetComputeRoot32BitConstants(
            0U, static_cast<UINT>(constants.size()), constants.data(), 0U);
        command_list_->SetComputeRootDescriptorTable(
            1U, descriptor_heap_->GetGPUDescriptorHandleForHeapStart());
        command_list_->Dispatch(
            plan.header.dispatch_x, plan.header.dispatch_y, 1U);

        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = output_.resource.Get();
        command_list_->ResourceBarrier(1U, &uav);
        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition.pResource = output_.resource.Get();
        to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        command_list_->ResourceBarrier(1U, &to_copy);
        command_list_->CopyBufferRegion(
            readback_.Get(), 0U, output_.resource.Get(), 0U,
            plan.header.output_bytes);
        std::swap(to_copy.Transition.StateBefore, to_copy.Transition.StateAfter);
        command_list_->ResourceBarrier(1U, &to_copy);

        result = command_list_->Close();
        if (FAILED(result)) {
            atlas_state_ = atlas_state_before;
            return fail(error, NativeShaderExecutionErrorKind::CommandEncodingFailed,
                        "Direct3D 12 shader command list close failed", result);
        }
        ID3D12CommandList* lists[] = {command_list_.Get()};
        queue_->ExecuteCommandLists(1U, lists);
        const std::uint64_t signal = next_fence_value_++;
        result = queue_->Signal(fence_.Get(), signal);
        if (FAILED(result)) {
            snapshot_.device_lost_events += 1U;
            return fail(error, NativeShaderExecutionErrorKind::DeviceLost,
                        "Direct3D 12 shader queue signal failed", result);
        }
        if (!wait_for_fence_locked(signal, error)) {
            return false;
        }
        atlas_generations_ = pending_generations;
        snapshot_.atlas_uploads += pending_upload_count;

        void* mapped = nullptr;
        D3D12_RANGE read_range{0U, static_cast<SIZE_T>(plan.header.output_bytes)};
        result = readback_->Map(0U, &read_range, &mapped);
        if (FAILED(result) || mapped == nullptr) {
            return fail(error, NativeShaderExecutionErrorKind::ReadbackFailed,
                        "Direct3D 12 shader readback mapping failed", result);
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
                D3D12_RANGE written{0U, 0U};
                readback_->Unmap(0U, &written);
                return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                            "Direct3D 12 shader readback allocation failed");
            }
        }
        D3D12_RANGE written{0U, 0U};
        readback_->Unmap(0U, &written);
        if ((request.flags & kNativeShaderExecutionRequireExactReadback) != 0U &&
            request.expected_readback_checksum != 0U &&
            checksum != request.expected_readback_checksum) {
            return fail(error, NativeShaderExecutionErrorKind::ReadbackMismatch,
                        "Direct3D 12 GPU readback differs from reference checksum");
        }

        *receipt = {};
        receipt->api_kind = NativeGpuApiKind::Direct3D12;
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
        receipt->readback_checksum = checksum;
        receipt->output_bytes = plan.header.output_bytes;

        snapshot_.executions += 1U;
        snapshot_.readbacks += 1U;
        snapshot_.last_submitted_fence_value = signal;
        snapshot_.completed_fence_value = signal;
        snapshot_.resident_atlas_pages = plan.header.atlas_binding_count;
        if (atlas_bytes_ > (std::numeric_limits<std::uint64_t>::max)() -
                output_.capacity) {
            snapshot_.current_device_bytes =
                (std::numeric_limits<std::uint64_t>::max)();
        } else {
            snapshot_.current_device_bytes = atlas_bytes_ + output_.capacity;
        }
        snapshot_.peak_device_bytes = std::max(
            snapshot_.peak_device_bytes, snapshot_.current_device_bytes);
        snapshot_.current_staging_bytes = 0U;
        const std::array<std::uint64_t, 4U> staging_parts{
            commands_.capacity,
            fills_.capacity,
            glyphs_.capacity,
            scissors_.capacity};
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
                        "Direct3D 12 shader completion fence is outside timeline");
        }
        if (!wait_for_fence_locked(completed_fence_value, error)) {
            return false;
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
    struct BufferSlot final {
        ComPtr<ID3D12Resource> resource;
        std::uint64_t capacity{0U};
        std::uint32_t stride{0U};
    };

    HRESULT compile_pipeline_locked(
        NativeShaderExecutionError* error) noexcept {
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG;
#endif
        ComPtr<ID3DBlob> shader;
        ComPtr<ID3DBlob> diagnostics;
        HRESULT result = D3DCompile(
            native_shader_hlsl_source().data(),
            native_shader_hlsl_source().size(),
            "zevryon-native-shader", nullptr, nullptr,
            "main", "cs_5_1", flags, 0U,
            &shader, &diagnostics);
        if (FAILED(result)) {
            (void)fail(error, NativeShaderExecutionErrorKind::ShaderCompilationFailed,
                       "Direct3D 12 compute shader compilation failed", result);
            return result;
        }

        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 6U;
        ranges[0].BaseShaderRegister = 0U;
        ranges[0].RegisterSpace = 0U;
        ranges[0].OffsetInDescriptorsFromTableStart = 0U;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1U;
        ranges[1].BaseShaderRegister = 0U;
        ranges[1].RegisterSpace = 0U;
        ranges[1].OffsetInDescriptorsFromTableStart = 6U;
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.ShaderRegister = 0U;
        parameters[0].Constants.RegisterSpace = 0U;
        parameters[0].Constants.Num32BitValues = 8U;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 2U;
        parameters[1].DescriptorTable.pDescriptorRanges = ranges;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC root_description{};
        root_description.NumParameters = 2U;
        root_description.pParameters = parameters;
        root_description.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> serialized;
        result = D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized, &diagnostics);
        if (FAILED(result)) {
            fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                 "Direct3D 12 root signature serialization failed", result);
            return result;
        }
        result = device_->CreateRootSignature(
            0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&root_signature_));
        if (FAILED(result)) {
            fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                 "Direct3D 12 root signature creation failed", result);
            return result;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description{};
        pipeline_description.pRootSignature = root_signature_.Get();
        pipeline_description.CS.pShaderBytecode = shader->GetBufferPointer();
        pipeline_description.CS.BytecodeLength = shader->GetBufferSize();
        result = device_->CreateComputePipelineState(
            &pipeline_description, IID_PPV_ARGS(&pipeline_));
        if (FAILED(result)) {
            fail(error, NativeShaderExecutionErrorKind::PipelineCreationFailed,
                 "Direct3D 12 compute pipeline creation failed", result);
        }
        return result;
    }

    bool ensure_upload_buffer_locked(
        BufferSlot* slot,
        std::uint64_t required,
        std::uint32_t stride,
        NativeShaderExecutionError* error) noexcept {
        if (slot == nullptr) {
            return false;
        }
        required = std::max<std::uint64_t>(required, stride);
        if (slot->resource != nullptr && slot->capacity >= required &&
            slot->stride == stride) {
            return true;
        }
        const D3D12_HEAP_PROPERTIES heap =
            heap_properties(D3D12_HEAP_TYPE_UPLOAD);
        const D3D12_RESOURCE_DESC description = buffer_description(required);
        ComPtr<ID3D12Resource> replacement;
        const HRESULT result = device_->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&replacement));
        if (FAILED(result)) {
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "Direct3D 12 shader upload buffer allocation failed", result);
        }
        slot->resource = std::move(replacement);
        slot->capacity = required;
        slot->stride = stride;
        return true;
    }

    bool ensure_output_locked(
        std::uint64_t required,
        NativeShaderExecutionError* error) noexcept {
        required = std::max<std::uint64_t>(required, 4U);
        if (output_.resource != nullptr && output_.capacity >= required &&
            readback_ != nullptr && readback_capacity_ >= required) {
            return true;
        }
        const D3D12_HEAP_PROPERTIES default_heap =
            heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC output_description = buffer_description(
            required, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ComPtr<ID3D12Resource> output;
        HRESULT result = device_->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &output_description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&output));
        if (FAILED(result)) {
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "Direct3D 12 shader output buffer allocation failed", result);
        }
        const D3D12_HEAP_PROPERTIES readback_heap =
            heap_properties(D3D12_HEAP_TYPE_READBACK);
        const D3D12_RESOURCE_DESC readback_description = buffer_description(required);
        ComPtr<ID3D12Resource> readback;
        result = device_->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback));
        if (FAILED(result)) {
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "Direct3D 12 shader readback buffer allocation failed", result);
        }
        output_.resource = std::move(output);
        output_.capacity = required;
        output_.stride = 4U;
        readback_ = std::move(readback);
        readback_capacity_ = required;
        return true;
    }

    bool ensure_descriptor_heap_locked(
        NativeShaderExecutionError* error) noexcept {
        if (descriptor_heap_ != nullptr) {
            return true;
        }
        D3D12_DESCRIPTOR_HEAP_DESC description{};
        description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        description.NumDescriptors = kDescriptorCount;
        description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        const HRESULT result = device_->CreateDescriptorHeap(
            &description, IID_PPV_ARGS(&descriptor_heap_));
        if (FAILED(result)) {
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "Direct3D 12 shader descriptor heap creation failed", result);
        }
        descriptor_increment_ = device_->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return true;
    }

    bool ensure_resources_locked(
        const NativeShaderDispatchPlan& plan,
        NativeShaderExecutionError* error) noexcept {
        return ensure_upload_buffer_locked(
                   &commands_, plan.header.command_bytes,
                   sizeof(NativeShaderDrawRecord), error) &&
            ensure_upload_buffer_locked(
                   &fills_, plan.header.fill_bytes,
                   sizeof(NativeShaderFillRecord), error) &&
            ensure_upload_buffer_locked(
                   &glyphs_, plan.header.glyph_bytes,
                   sizeof(NativeShaderGlyphRecord), error) &&
            ensure_upload_buffer_locked(
                   &scissors_, plan.header.scissor_bytes,
                   sizeof(NativeShaderScissorRecord), error) &&
            ensure_upload_buffer_locked(&dummy_, 16U, 16U, error) &&
            ensure_output_locked(plan.header.output_bytes, error) &&
            ensure_descriptor_heap_locked(error) &&
            ensure_atlas_texture_locked(plan, error);
    }

    bool ensure_atlas_texture_locked(
        const NativeShaderDispatchPlan& plan,
        NativeShaderExecutionError* error) noexcept {
        std::uint32_t width = 1U;
        std::uint32_t height = 1U;
        for (const NativeShaderAtlasBinding& binding : plan.atlas_bindings) {
            width = std::max(width, static_cast<std::uint32_t>(binding.width));
            height = std::max(height, static_cast<std::uint32_t>(binding.height));
        }
        if (atlas_texture_ != nullptr && atlas_width_ >= width &&
            atlas_height_ >= height && atlas_layers_ >= limits_.maximum_atlas_pages) {
            return true;
        }
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = width;
        description.Height = height;
        description.DepthOrArraySize =
            static_cast<UINT16>(limits_.maximum_atlas_pages);
        description.MipLevels = 1U;
        description.Format = DXGI_FORMAT_R8G8B8A8_UINT;
        description.SampleDesc.Count = 1U;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        description.Flags = D3D12_RESOURCE_FLAG_NONE;
        const D3D12_HEAP_PROPERTIES heap =
            heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        ComPtr<ID3D12Resource> texture;
        const HRESULT result = device_->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&texture));
        if (FAILED(result)) {
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "Direct3D 12 persistent atlas texture allocation failed", result);
        }
        atlas_texture_ = std::move(texture);
        atlas_width_ = width;
        atlas_height_ = height;
        atlas_layers_ = limits_.maximum_atlas_pages;
        atlas_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
        atlas_generations_.fill(0U);
        atlas_bytes_ = static_cast<std::uint64_t>(width) * height *
            atlas_layers_ * 4U;
        return true;
    }

    bool upload_atlas_locked(
        const NativeShaderDispatchPlan& plan,
        const ShaderAtlasResidency& atlas,
        std::vector<ComPtr<ID3D12Resource>>* keep_alive,
        std::array<std::uint32_t, 16U>* pending_generations,
        std::uint64_t* pending_upload_count,
        NativeShaderExecutionError* error) noexcept {
        if (keep_alive == nullptr || pending_generations == nullptr ||
            pending_upload_count == nullptr) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "null Direct3D 12 atlas publication target");
        }
        bool has_upload = false;
        if (atlas_state_ != D3D12_RESOURCE_STATE_COPY_DEST) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = atlas_texture_.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = atlas_state_;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            command_list_->ResourceBarrier(1U, &barrier);
            atlas_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
        }
        for (const NativeShaderAtlasBinding& binding : plan.atlas_bindings) {
            if ((*pending_generations)[binding.texture_layer] == binding.page_generation) {
                continue;
            }
            const ShaderAtlasResidentPage* page = atlas.find(
                binding.page_index, binding.page_generation);
            if (page == nullptr || page->canonical_bgra.size() != binding.resident_bytes) {
                return fail(error, NativeShaderExecutionErrorKind::InvalidAtlasReference,
                            "Direct3D 12 atlas page is no longer resident");
            }
            D3D12_RESOURCE_DESC subresource_description = atlas_texture_->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 total_bytes = 0U;
            device_->GetCopyableFootprints(
                &subresource_description, binding.texture_layer, 1U, 0U,
                &footprint, nullptr, nullptr, &total_bytes);
            const D3D12_HEAP_PROPERTIES heap =
                heap_properties(D3D12_HEAP_TYPE_UPLOAD);
            const D3D12_RESOURCE_DESC upload_description =
                buffer_description(total_bytes);
            ComPtr<ID3D12Resource> upload;
            HRESULT result = device_->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &upload_description,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&upload));
            if (FAILED(result)) {
                return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                            "Direct3D 12 atlas upload allocation failed", result);
            }
            void* mapped = nullptr;
            result = upload->Map(0U, nullptr, &mapped);
            if (FAILED(result) || mapped == nullptr) {
                return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                            "Direct3D 12 atlas upload mapping failed", result);
            }
            auto* destination = static_cast<std::byte*>(mapped) + footprint.Offset;
            for (std::uint32_t row = 0U; row < binding.height; ++row) {
                std::memcpy(
                    destination + static_cast<std::size_t>(row) *
                        footprint.Footprint.RowPitch,
                    page->canonical_bgra.data() +
                        static_cast<std::size_t>(row) * binding.row_bytes,
                    binding.row_bytes);
            }
            upload->Unmap(0U, nullptr);
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprint;
            D3D12_TEXTURE_COPY_LOCATION destination_location{};
            destination_location.pResource = atlas_texture_.Get();
            destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination_location.SubresourceIndex = binding.texture_layer;
            command_list_->CopyTextureRegion(
                &destination_location, 0U, 0U, 0U, &source, nullptr);
            try {
                keep_alive->push_back(upload);
            } catch (const std::bad_alloc&) {
                return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                            "Direct3D 12 atlas keep-alive allocation failed");
            } catch (...) {
                return fail(error, NativeShaderExecutionErrorKind::SubmissionFailed,
                            "unexpected Direct3D 12 atlas keep-alive failure");
            }
            (*pending_generations)[binding.texture_layer] = binding.page_generation;
            *pending_upload_count += 1U;
            has_upload = true;
        }
        if (has_upload || atlas_state_ == D3D12_RESOURCE_STATE_COPY_DEST) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = atlas_texture_.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            command_list_->ResourceBarrier(1U, &barrier);
            atlas_state_ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        return true;
    }

    bool copy_upload_locked(
        const BufferSlot& slot,
        const void* source,
        std::uint64_t bytes,
        NativeShaderExecutionError* error) noexcept {
        if (bytes == 0U) {
            return true;
        }
        if (slot.resource == nullptr || source == nullptr || bytes > slot.capacity) {
            return fail(error, NativeShaderExecutionErrorKind::InvalidInput,
                        "invalid Direct3D 12 shader upload span");
        }
        void* mapped = nullptr;
        const HRESULT result = slot.resource->Map(0U, nullptr, &mapped);
        if (FAILED(result) || mapped == nullptr) {
            return fail(error, NativeShaderExecutionErrorKind::AllocationFailed,
                        "Direct3D 12 shader upload mapping failed", result);
        }
        std::memcpy(mapped, source, static_cast<std::size_t>(bytes));
        slot.resource->Unmap(0U, nullptr);
        return true;
    }

    void create_structured_srv(
        const BufferSlot& slot,
        UINT index,
        std::uint64_t bytes) noexcept {
        D3D12_SHADER_RESOURCE_VIEW_DESC description{};
        description.Format = DXGI_FORMAT_UNKNOWN;
        description.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        description.Buffer.FirstElement = 0U;
        description.Buffer.NumElements = static_cast<UINT>(
            std::max<std::uint64_t>(1U, bytes / slot.stride));
        description.Buffer.StructureByteStride = slot.stride;
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * descriptor_increment_;
        device_->CreateShaderResourceView(slot.resource.Get(), &description, handle);
    }

    void create_descriptors_locked(
        const NativeShaderDispatchPlan& plan) noexcept {
        create_structured_srv(commands_, 0U, plan.header.command_bytes);
        create_structured_srv(fills_, 1U, plan.header.fill_bytes);
        create_structured_srv(glyphs_, 2U, plan.header.glyph_bytes);
        create_structured_srv(scissors_, 3U, plan.header.scissor_bytes);
        create_structured_srv(dummy_, 4U, 16U);
        D3D12_SHADER_RESOURCE_VIEW_DESC atlas_description{};
        atlas_description.Format = DXGI_FORMAT_R8G8B8A8_UINT;
        atlas_description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        atlas_description.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        atlas_description.Texture2DArray.MipLevels = 1U;
        atlas_description.Texture2DArray.ArraySize = atlas_layers_;
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(5U) * descriptor_increment_;
        device_->CreateShaderResourceView(
            atlas_texture_.Get(), &atlas_description, handle);
        D3D12_UNORDERED_ACCESS_VIEW_DESC output_description{};
        output_description.Format = DXGI_FORMAT_UNKNOWN;
        output_description.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        output_description.Buffer.FirstElement = 0U;
        output_description.Buffer.NumElements = static_cast<UINT>(
            plan.header.output_bytes / 4U);
        output_description.Buffer.StructureByteStride = 4U;
        handle.ptr += descriptor_increment_;
        device_->CreateUnorderedAccessView(
            output_.resource.Get(), nullptr, &output_description, handle);
    }

    bool wait_for_fence_locked(
        std::uint64_t value,
        NativeShaderExecutionError* error) noexcept {
        if (fence_->GetCompletedValue() >= value) {
            return true;
        }
        const HRESULT result = fence_->SetEventOnCompletion(value, fence_event_);
        if (FAILED(result)) {
            return fail(error, NativeShaderExecutionErrorKind::SubmissionFailed,
                        "Direct3D 12 shader fence event registration failed", result);
        }
        const DWORD waited = WaitForSingleObject(fence_event_, kFenceTimeoutMs);
        if (waited != WAIT_OBJECT_0) {
            return fail(error, NativeShaderExecutionErrorKind::SubmissionFailed,
                        "Direct3D 12 shader fence wait timed out",
                        HRESULT_FROM_WIN32(waited == WAIT_FAILED
                            ? GetLastError() : ERROR_TIMEOUT));
        }
        return true;
    }

    void shutdown_locked() noexcept {
        if (queue_ != nullptr && fence_ != nullptr) {
            const std::uint64_t signal = next_fence_value_++;
            if (SUCCEEDED(queue_->Signal(fence_.Get(), signal))) {
                NativeShaderExecutionError ignored;
                (void)wait_for_fence_locked(signal, &ignored);
            }
        }
        if (fence_event_ != nullptr) {
            CloseHandle(fence_event_);
            fence_event_ = nullptr;
        }
        commands_ = {};
        fills_ = {};
        glyphs_ = {};
        scissors_ = {};
        dummy_ = {};
        output_ = {};
        readback_.Reset();
        readback_capacity_ = 0U;
        atlas_texture_.Reset();
        atlas_generations_.fill(0U);
        atlas_width_ = 0U;
        atlas_height_ = 0U;
        atlas_layers_ = 0U;
        atlas_bytes_ = 0U;
        atlas_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
        descriptor_heap_.Reset();
        descriptor_increment_ = 0U;
        command_list_.Reset();
        allocator_.Reset();
        pipeline_.Reset();
        root_signature_.Reset();
        fence_.Reset();
        queue_.Reset();
        device_.Reset();
        snapshot_ = {};
        limits_ = {};
        next_fence_value_ = 1U;
        configured_ = false;
    }

    mutable std::mutex mutex_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12RootSignature> root_signature_;
    ComPtr<ID3D12PipelineState> pipeline_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> command_list_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_{nullptr};
    ComPtr<ID3D12DescriptorHeap> descriptor_heap_;
    UINT descriptor_increment_{0U};
    BufferSlot commands_;
    BufferSlot fills_;
    BufferSlot glyphs_;
    BufferSlot scissors_;
    BufferSlot dummy_;
    BufferSlot output_;
    ComPtr<ID3D12Resource> readback_;
    std::uint64_t readback_capacity_{0U};
    ComPtr<ID3D12Resource> atlas_texture_;
    D3D12_RESOURCE_STATES atlas_state_{D3D12_RESOURCE_STATE_COPY_DEST};
    std::array<std::uint32_t, 16U> atlas_generations_{};
    std::uint32_t atlas_width_{0U};
    std::uint32_t atlas_height_{0U};
    std::uint32_t atlas_layers_{0U};
    std::uint64_t atlas_bytes_{0U};
    NativeShaderExecutionLimits limits_;
    NativeShaderExecutionSnapshot snapshot_;
    std::uint64_t next_fence_value_{1U};
    bool configured_{false};
};

} // namespace

std::unique_ptr<NativeShaderExecutionApi>
make_direct3d12_native_shader_execution_api() noexcept {
    try {
        return std::make_unique<Direct3D12NativeShaderExecutionApi>();
    } catch (...) {
        return nullptr;
    }
}

} // namespace zevryon::text

#endif // ZEVRYON_HAS_D3D12_NATIVE_SHADER
