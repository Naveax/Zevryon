#include "native_gpu_sdk_execution.hpp"

#if defined(ZEVRYON_HAS_D3D12_SDK)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

namespace zevryon::text {
namespace {

using Microsoft::WRL::ComPtr;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr DWORD kFenceTimeoutMs = 5000U;

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

DXGI_FORMAT map_format(GpuSurfaceFormat format) noexcept {
    return format == GpuSurfaceFormat::Rgba8Unorm
        ? DXGI_FORMAT_R8G8B8A8_UNORM
        : DXGI_FORMAT_B8G8R8A8_UNORM;
}

class Direct3D12NativeGpuSdkApi final : public NativeGpuSdkApi {
public:
    Direct3D12NativeGpuSdkApi() noexcept {
        snapshot_.probe.api_kind = NativeGpuApiKind::Direct3D12;
        snapshot_.probe.availability = NativeGpuSdkAvailability::CompileOnly;
        snapshot_.probe.api_major = 12U;
        snapshot_.probe.flags = kNativeGpuSdkOffscreenSurface |
                                kNativeGpuSdkWindowSurface;
    }

    ~Direct3D12NativeGpuSdkApi() override { shutdown(); }

    NativeGpuApiKind kind() const noexcept override {
        return NativeGpuApiKind::Direct3D12;
    }

    NativeGpuSdkProbe probe() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_.probe;
    }

    bool export_context(
        NativeGpuSdkContextHandle* context,
        NativeGpuSdkError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (context == nullptr) {
            return fail(error, NativeGpuSdkErrorKind::InvalidInput,
                        "Direct3D 12 context output is null");
        }
        if (!initialized_ || factory_ == nullptr || adapter_ == nullptr ||
            device_ == nullptr || queue_ == nullptr) {
            *context = {};
            return fail(error, NativeGpuSdkErrorKind::RuntimeUnavailable,
                        "Direct3D 12 device context is not initialized");
        }
        *context = {};
        context->api_kind = NativeGpuApiKind::Direct3D12;
        context->flags =
            kNativeGpuSdkContextDeviceValid |
            kNativeGpuSdkContextGraphicsQueueValid |
            kNativeGpuSdkContextPresentQueueValid |
            kNativeGpuSdkContextSharedGraphicsPresentQueue;
        if ((snapshot_.probe.flags & kNativeGpuSdkSoftwareDevice) != 0U) {
            context->flags |= kNativeGpuSdkContextSoftwareDevice;
        }
        context->device_generation = config_.device_generation;
        context->runtime_generation = config_.runtime_generation;
        context->instance_or_factory = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(factory_.Get()));
        context->physical_device_or_adapter = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(adapter_.Get()));
        context->device = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(device_.Get()));
        context->graphics_queue = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(queue_.Get()));
        context->present_queue = context->graphics_queue;
        return true;
    }

    bool initialize(
        const NativeGpuSdkConfig& config,
        NativeGpuSdkError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (config.api_kind != NativeGpuApiKind::Direct3D12 ||
            config.device_generation == 0U || config.runtime_generation == 0U ||
            config.window.system != NativeWindowSystem::Headless) {
            return fail(error, NativeGpuSdkErrorKind::InvalidInput,
                        "invalid Direct3D 12 SDK configuration");
        }
        shutdown_locked();

        UINT factory_flags = 0U;
        HRESULT result = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory_));
        if (FAILED(result)) {
            return fail(error, NativeGpuSdkErrorKind::RuntimeUnavailable,
                        "CreateDXGIFactory2 failed", result);
        }

        DXGI_ADAPTER_DESC1 selected_desc{};
        bool found = false;
        for (UINT index = 0U;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            result = factory_->EnumAdapters1(index, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(result)) {
                continue;
            }
            DXGI_ADAPTER_DESC1 desc{};
            candidate->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U) {
                continue;
            }
            result = D3D12CreateDevice(
                candidate.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
            if (SUCCEEDED(result)) {
                adapter_ = candidate;
                selected_desc = desc;
                found = true;
                break;
            }
        }
        bool software_device = false;
        if (!found && config.allow_software_device != 0U) {
            ComPtr<IDXGIAdapter> warp_adapter;
            result = factory_->EnumWarpAdapter(IID_PPV_ARGS(&warp_adapter));
            if (SUCCEEDED(result)) {
                result = D3D12CreateDevice(
                    warp_adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
                if (SUCCEEDED(result)) {
                    software_device = true;
                    found = true;
                    ComPtr<IDXGIAdapter1> warp_adapter1;
                    if (SUCCEEDED(warp_adapter.As(&warp_adapter1))) {
                        adapter_ = warp_adapter1;
                        adapter_->GetDesc1(&selected_desc);
                    }
                }
            }
        }
        if (!found) {
            shutdown_locked();
            return fail(error, NativeGpuSdkErrorKind::DeviceCreationFailed,
                        "no Direct3D 12 hardware or WARP device is available", result);
        }

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        result = device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_));
        if (FAILED(result)) {
            shutdown_locked();
            return fail(error, NativeGpuSdkErrorKind::QueueCreationFailed,
                        "ID3D12Device::CreateCommandQueue failed", result);
        }
        result = device_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_));
        if (FAILED(result)) {
            shutdown_locked();
            return fail(error, NativeGpuSdkErrorKind::QueueCreationFailed,
                        "ID3D12Device::CreateCommandAllocator failed", result);
        }
        result = device_->CreateCommandList(
            0U,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocator_.Get(),
            nullptr,
            IID_PPV_ARGS(&command_list_));
        if (FAILED(result)) {
            shutdown_locked();
            return fail(error, NativeGpuSdkErrorKind::QueueCreationFailed,
                        "ID3D12Device::CreateCommandList failed", result);
        }
        result = command_list_->Close();
        if (FAILED(result)) {
            shutdown_locked();
            return fail(error, NativeGpuSdkErrorKind::QueueCreationFailed,
                        "initial Direct3D 12 command list close failed", result);
        }
        result = device_->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        if (FAILED(result)) {
            shutdown_locked();
            return fail(error, NativeGpuSdkErrorKind::QueueCreationFailed,
                        "ID3D12Device::CreateFence failed", result);
        }
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence_event_ == nullptr) {
            const HRESULT event_error = HRESULT_FROM_WIN32(GetLastError());
            shutdown_locked();
            return fail(error, NativeGpuSdkErrorKind::QueueCreationFailed,
                        "CreateEventW for the Direct3D 12 fence failed", event_error);
        }

        config_ = config;
        snapshot_.config = config;
        snapshot_.probe.availability = NativeGpuSdkAvailability::RuntimeReady;
        snapshot_.probe.flags = kNativeGpuSdkRealDevice |
                                kNativeGpuSdkOffscreenSurface |
                                kNativeGpuSdkWindowSurface;
        if (software_device) {
            snapshot_.probe.flags |= kNativeGpuSdkSoftwareDevice;
        }
        snapshot_.probe.vendor_id = selected_desc.VendorId;
        snapshot_.probe.device_id = selected_desc.DeviceId;
        snapshot_.probe.dedicated_video_memory_bytes = selected_desc.DedicatedVideoMemory;
        snapshot_.probe.shared_system_memory_bytes = selected_desc.SharedSystemMemory;
        snapshot_.probe.runtime_generation = config.runtime_generation;
        std::uint64_t checksum = kFnvOffset;
        hash_value(&checksum, selected_desc.VendorId);
        hash_value(&checksum, selected_desc.DeviceId);
        hash_value(&checksum, selected_desc.AdapterLuid.HighPart);
        hash_value(&checksum, selected_desc.AdapterLuid.LowPart);
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
            image_count > resources_.size()) {
            return fail(error, NativeGpuSdkErrorKind::InvalidInput,
                        "invalid Direct3D 12 offscreen surface configuration");
        }
        if (!wait_for_gpu_locked(error)) {
            return false;
        }
        destroy_resources_locked();

        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = image_count;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        HRESULT result = device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap_));
        if (FAILED(result)) {
            return fail(error, NativeGpuSdkErrorKind::ResourceAllocationFailed,
                        "Direct3D 12 RTV descriptor heap creation failed", result);
        }
        rtv_increment_ = device_->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        const std::uint64_t bytes_per_image =
            static_cast<std::uint64_t>(surface.width) * surface.height * 4U;
        if (surface.width != 0U && bytes_per_image / surface.width / 4U != surface.height) {
            destroy_resources_locked();
            return fail(error, NativeGpuSdkErrorKind::AggregateOverflow,
                        "Direct3D 12 offscreen image byte count overflowed");
        }
        if (bytes_per_image > config_.limits.maximum_device_local_bytes / image_count) {
            destroy_resources_locked();
            return fail(error, NativeGpuSdkErrorKind::ResourceBudgetExceeded,
                        "Direct3D 12 offscreen resource ring exceeds device-local budget");
        }
        for (std::uint32_t index = 0U; index < image_count; ++index) {
            D3D12_HEAP_PROPERTIES heap_properties{};
            heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            heap_properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            heap_properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            heap_properties.CreationNodeMask = 1U;
            heap_properties.VisibleNodeMask = 1U;

            D3D12_RESOURCE_DESC resource_desc{};
            resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            resource_desc.Alignment = 0U;
            resource_desc.Width = surface.width;
            resource_desc.Height = surface.height;
            resource_desc.DepthOrArraySize = 1U;
            resource_desc.MipLevels = 1U;
            resource_desc.Format = map_format(surface.format);
            resource_desc.SampleDesc.Count = 1U;
            resource_desc.SampleDesc.Quality = 0U;
            resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            D3D12_CLEAR_VALUE clear_value{};
            clear_value.Format = resource_desc.Format;
            clear_value.Color[0] = 0.0F;
            clear_value.Color[1] = 0.0F;
            clear_value.Color[2] = 0.0F;
            clear_value.Color[3] = 1.0F;
            result = device_->CreateCommittedResource(
                &heap_properties,
                D3D12_HEAP_FLAG_NONE,
                &resource_desc,
                D3D12_RESOURCE_STATE_COMMON,
                &clear_value,
                IID_PPV_ARGS(&resources_[index].resource));
            if (FAILED(result)) {
                destroy_resources_locked();
                return fail(error, NativeGpuSdkErrorKind::ResourceAllocationFailed,
                            "Direct3D 12 offscreen resource creation failed", result);
            }
            device_->CreateRenderTargetView(resources_[index].resource.Get(), nullptr, rtv);
            resources_[index].rtv = rtv;
            resources_[index].state = D3D12_RESOURCE_STATE_COMMON;
            resources_[index].native_resource_id = next_resource_id_++;
            resources_[index].generation = next_image_generation_++;
            resources_[index].allocated_bytes = bytes_per_image;
            rtv.ptr += rtv_increment_;
        }
        surface_ = surface;
        image_count_ = image_count;
        next_image_index_ = 0U;
        snapshot_.surface = surface;
        snapshot_.configured_image_count = image_count;
        snapshot_.configured_surfaces += 1U;
        snapshot_.current_device_local_bytes = bytes_per_image * image_count;
        snapshot_.peak_device_local_bytes = std::max(
            snapshot_.peak_device_local_bytes,
            snapshot_.current_device_local_bytes);
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
                        "invalid or stale Direct3D 12 offscreen acquire request");
        }
        const std::uint32_t index = next_image_index_++ % image_count_;
        image->image.device_generation = config_.device_generation;
        image->image.surface_id = surface.surface_id;
        image->image.surface_generation = surface.generation_id;
        image->image.image_generation = resources_[index].generation;
        image->image.image_index = index;
        image->image.flags = 0U;
        image->driver_generation = config_.runtime_generation;
        image->native_resource_id = resources_[index].native_resource_id;
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
            submission.api_kind != NativeGpuApiKind::Direct3D12 ||
            !(submission.surface == surface_) ||
            submission.image.image.device_generation != config_.device_generation ||
            submission.image.driver_generation != config_.runtime_generation ||
            submission.image.image.image_index >= image_count_) {
            snapshot_.stale_rejections += 1U;
            return fail(error, NativeGpuSdkErrorKind::StaleGeneration,
                        "Direct3D 12 submission references stale execution state");
        }
        if (submission.commands.size() > config_.limits.maximum_submission_commands ||
            submission.descriptors.size() > config_.limits.maximum_descriptors) {
            return fail(error, NativeGpuSdkErrorKind::ResourceBudgetExceeded,
                        "Direct3D 12 submission exceeds bounded command or descriptor limits");
        }
        const std::uint32_t image_index = submission.image.image.image_index;
        ResourceSlot& slot = resources_[image_index];
        if (submission.image.native_resource_id != slot.native_resource_id ||
            submission.image.image.image_generation != slot.generation) {
            snapshot_.stale_rejections += 1U;
            return fail(error, NativeGpuSdkErrorKind::StaleGeneration,
                        "Direct3D 12 acquired image generation is stale");
        }

        HRESULT result = allocator_->Reset();
        if (FAILED(result)) {
            return fail(error, NativeGpuSdkErrorKind::CommandEncodingFailed,
                        "ID3D12CommandAllocator::Reset failed", result);
        }
        result = command_list_->Reset(allocator_.Get(), nullptr);
        if (FAILED(result)) {
            return fail(error, NativeGpuSdkErrorKind::CommandEncodingFailed,
                        "ID3D12GraphicsCommandList::Reset failed", result);
        }
        if (slot.state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = slot.resource.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = slot.state;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            command_list_->ResourceBarrier(1U, &barrier);
            slot.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        const FLOAT color[4] = {
            static_cast<FLOAT>((submission.encoded_checksum >> 0U) & 0xFFU) / 255.0F,
            static_cast<FLOAT>((submission.encoded_checksum >> 8U) & 0xFFU) / 255.0F,
            static_cast<FLOAT>((submission.encoded_checksum >> 16U) & 0xFFU) / 255.0F,
            1.0F};
        command_list_->ClearRenderTargetView(slot.rtv, color, 0U, nullptr);
        D3D12_RESOURCE_BARRIER present_barrier{};
        present_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        present_barrier.Transition.pResource = slot.resource.Get();
        present_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        present_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        present_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        command_list_->ResourceBarrier(1U, &present_barrier);
        slot.state = D3D12_RESOURCE_STATE_COMMON;
        result = command_list_->Close();
        if (FAILED(result)) {
            return fail(error, NativeGpuSdkErrorKind::CommandEncodingFailed,
                        "ID3D12GraphicsCommandList::Close failed", result);
        }
        ID3D12CommandList* lists[] = {command_list_.Get()};
        queue_->ExecuteCommandLists(1U, lists);
        const std::uint64_t signal = next_fence_value_++;
        result = queue_->Signal(fence_.Get(), signal);
        if (FAILED(result)) {
            if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
                snapshot_.device_lost_events += 1U;
                return fail(error, NativeGpuSdkErrorKind::DeviceLost,
                            "Direct3D 12 device was lost during queue signal", result);
            }
            return fail(error, NativeGpuSdkErrorKind::SubmissionFailed,
                        "ID3D12CommandQueue::Signal failed", result);
        }
        if (!wait_for_fence_locked(signal, error)) {
            return false;
        }
        if (signal <= snapshot_.last_submitted_fence_value) {
            return fail(error, NativeGpuSdkErrorKind::FenceRegression,
                        "Direct3D 12 fence timeline regressed");
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

        receipt->api_kind = NativeGpuApiKind::Direct3D12;
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
                        "Direct3D 12 completion fence is outside the submitted timeline");
        }
        if (!wait_for_fence_locked(completed_fence_value, error)) {
            return false;
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
    struct ResourceSlot final {
        ComPtr<ID3D12Resource> resource;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
        std::uint64_t native_resource_id{0};
        std::uint64_t generation{0};
        std::uint64_t allocated_bytes{0};
    };

    bool wait_for_fence_locked(
        std::uint64_t value,
        NativeGpuSdkError* error) noexcept {
        if (value == 0U || fence_ == nullptr) {
            return true;
        }
        if (fence_->GetCompletedValue() >= value) {
            return true;
        }
        HRESULT result = fence_->SetEventOnCompletion(value, fence_event_);
        if (FAILED(result)) {
            return fail(error, NativeGpuSdkErrorKind::SubmissionFailed,
                        "ID3D12Fence::SetEventOnCompletion failed", result);
        }
        const DWORD wait_result = WaitForSingleObject(fence_event_, kFenceTimeoutMs);
        if (wait_result != WAIT_OBJECT_0) {
            return fail(error, NativeGpuSdkErrorKind::SubmissionFailed,
                        "Direct3D 12 fence wait timed out",
                        HRESULT_FROM_WIN32(wait_result == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT));
        }
        return true;
    }

    bool wait_for_gpu_locked(NativeGpuSdkError* error) noexcept {
        if (queue_ == nullptr || fence_ == nullptr) {
            return true;
        }
        const std::uint64_t signal = next_fence_value_++;
        const HRESULT result = queue_->Signal(fence_.Get(), signal);
        if (FAILED(result)) {
            return fail(error, NativeGpuSdkErrorKind::SubmissionFailed,
                        "Direct3D 12 idle signal failed", result);
        }
        return wait_for_fence_locked(signal, error);
    }

    void destroy_resources_locked() noexcept {
        for (ResourceSlot& slot : resources_) {
            slot.resource.Reset();
            slot.rtv = {};
            slot.state = D3D12_RESOURCE_STATE_COMMON;
            slot.native_resource_id = 0U;
            slot.generation = 0U;
            slot.allocated_bytes = 0U;
        }
        rtv_heap_.Reset();
        rtv_increment_ = 0U;
        image_count_ = 0U;
        next_image_index_ = 0U;
        snapshot_.configured_image_count = 0U;
        snapshot_.current_device_local_bytes = 0U;
        snapshot_.surface = {};
        surface_ = {};
    }

    void shutdown_locked() noexcept {
        NativeGpuSdkError ignored;
        (void)wait_for_gpu_locked(&ignored);
        destroy_resources_locked();
        command_list_.Reset();
        allocator_.Reset();
        queue_.Reset();
        fence_.Reset();
        device_.Reset();
        adapter_.Reset();
        factory_.Reset();
        if (fence_event_ != nullptr) {
            CloseHandle(fence_event_);
            fence_event_ = nullptr;
        }
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
    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> command_list_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    HANDLE fence_event_{nullptr};
    std::array<ResourceSlot, 16U> resources_{};
    UINT rtv_increment_{0U};
    std::uint32_t image_count_{0U};
    std::uint32_t next_image_index_{0U};
    std::uint64_t next_image_generation_{1U};
    std::uint64_t next_resource_id_{1U};
    std::uint64_t next_fence_value_{1U};
    bool initialized_{false};
};

} // namespace

std::unique_ptr<NativeGpuSdkApi> make_direct3d12_native_gpu_sdk_api() noexcept {
    try {
        return std::make_unique<Direct3D12NativeGpuSdkApi>();
    } catch (...) {
        return nullptr;
    }
}

} // namespace zevryon::text

#endif // ZEVRYON_HAS_D3D12_SDK
