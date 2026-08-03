#include "native_shader_surface_d3d12.hpp"
#include "native_window_swapchain.hpp"

#if defined(ZEVRYON_HAS_D3D12_WINDOW_SWAPCHAIN)

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
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>

namespace zevryon::text {
namespace {

using Microsoft::WRL::ComPtr;
constexpr std::uint64_t kBytesPerPixel = 4U;
constexpr DWORD kFenceTimeoutMs = 5000U;

void clear_error(NativeWindowSwapchainError* error) noexcept {
    if (error != nullptr) {
        error->kind = NativeWindowSwapchainErrorKind::None;
        error->native_code = 0;
        error->message.clear();
    }
}

bool fail(
    NativeWindowSwapchainError* error,
    NativeWindowSwapchainErrorKind kind,
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

bool checked_surface_bytes(
    const GpuSurfaceDescriptor& surface,
    std::uint32_t image_count,
    std::uint64_t* bytes_per_image,
    std::uint64_t* total_bytes) noexcept {
    if (bytes_per_image == nullptr || total_bytes == nullptr ||
        surface.width == 0U || surface.height == 0U || image_count == 0U) {
        return false;
    }
    const std::uint64_t width = surface.width;
    const std::uint64_t height = surface.height;
    if (width > std::numeric_limits<std::uint64_t>::max() / height) {
        return false;
    }
    const std::uint64_t pixels = width * height;
    if (pixels > std::numeric_limits<std::uint64_t>::max() / kBytesPerPixel) {
        return false;
    }
    *bytes_per_image = pixels * kBytesPerPixel;
    if (*bytes_per_image >
        std::numeric_limits<std::uint64_t>::max() / image_count) {
        return false;
    }
    *total_bytes = *bytes_per_image * image_count;
    return true;
}

bool damage_rect_valid(
    const NativeDamageRect& rect,
    const GpuSurfaceDescriptor& surface) noexcept {
    if (rect.inline_start < 0 || rect.block_start < 0 ||
        rect.inline_size == 0U || rect.block_size == 0U) {
        return false;
    }
    const std::uint64_t x = static_cast<std::uint64_t>(rect.inline_start);
    const std::uint64_t y = static_cast<std::uint64_t>(rect.block_start);
    return x <= surface.width && y <= surface.height &&
        rect.inline_size <= static_cast<std::uint64_t>(surface.width) - x &&
        rect.block_size <= static_cast<std::uint64_t>(surface.height) - y;
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

class Direct3D12NativeWindowSwapchainApi final
    : public NativeWindowSwapchainApi {
public:
    Direct3D12NativeWindowSwapchainApi() noexcept {
        snapshot_.capabilities = default_native_window_swapchain_capabilities(
            NativeGpuApiKind::Direct3D12,
            NativeWindowSystem::Win32);
    }

    ~Direct3D12NativeWindowSwapchainApi() override { shutdown(); }

    NativeWindowSwapchainCapabilities capabilities() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_.capabilities;
    }

    bool configure(
        const NativeWindowSwapchainConfig& config,
        NativeWindowSwapchainError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (snapshot_.configured != 0U) {
            return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                        "Direct3D 12 window swapchain is already configured");
        }
        return configure_locked(config, false, error);
    }

    bool request_resize(
        const GpuSurfaceDescriptor& surface,
        NativeWindowSwapchainError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (snapshot_.configured == 0U ||
            surface.surface_id != snapshot_.config.surface.surface_id ||
            surface.generation_id <= snapshot_.config.surface.generation_id ||
            surface.width == 0U || surface.height == 0U ||
            surface.width > snapshot_.config.limits.maximum_width ||
            surface.height > snapshot_.config.limits.maximum_height ||
            surface.width > snapshot_.capabilities.maximum_width ||
            surface.height > snapshot_.capabilities.maximum_height) {
            snapshot_.stale_rejections += 1U;
            return fail(error, NativeWindowSwapchainErrorKind::StaleGeneration,
                        "Direct3D 12 resize does not advance a valid surface generation");
        }
        std::uint64_t bytes_per_image = 0U;
        std::uint64_t total_bytes = 0U;
        if (!checked_surface_bytes(
                surface,
                snapshot_.config.image_count,
                &bytes_per_image,
                &total_bytes)) {
            return fail(error, NativeWindowSwapchainErrorKind::ArithmeticOverflow,
                        "Direct3D 12 resized surface byte count overflowed");
        }
        if (total_bytes > snapshot_.config.limits.maximum_surface_bytes ||
            total_bytes > snapshot_.capabilities.maximum_surface_bytes ||
            bytes_per_image >
                snapshot_.config.limits.maximum_in_flight_bytes /
                    snapshot_.config.limits.maximum_frames_in_flight) {
            return fail(error, NativeWindowSwapchainErrorKind::ResourceBudgetExceeded,
                        "Direct3D 12 resized surface exceeds the configured budget");
        }
        snapshot_.pending_surface = surface;
        snapshot_.out_of_date = 1U;
        snapshot_.resize_requests += 1U;
        snapshot_.out_of_date_events += 1U;
        return true;
    }

    bool recreate(
        const NativeWindowSwapchainConfig& config,
        NativeWindowSwapchainError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (snapshot_.configured == 0U || snapshot_.out_of_date == 0U ||
            snapshot_.pending_surface.surface_id == 0U) {
            return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                        "Direct3D 12 swapchain recreation was not requested");
        }
        return configure_locked(config, true, error);
    }

    bool acquire(
        std::uint64_t ticket_id,
        NativeWindowSwapchainImage* image,
        NativeWindowAcquireStatus* status,
        NativeWindowSwapchainError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (image == nullptr || status == nullptr || ticket_id == 0U ||
            snapshot_.configured == 0U || swapchain_ == nullptr) {
            return fail(error, NativeWindowSwapchainErrorKind::AcquireFailed,
                        "invalid Direct3D 12 window acquire request");
        }
        if (snapshot_.device_lost != 0U) {
            *status = NativeWindowAcquireStatus::DeviceLost;
            return true;
        }
        if (snapshot_.out_of_date != 0U) {
            *status = NativeWindowAcquireStatus::OutOfDate;
            return true;
        }
        if (snapshot_.occluded != 0U) {
            const HRESULT test = swapchain_->Present(0U, DXGI_PRESENT_TEST);
            if (test == DXGI_STATUS_OCCLUDED) {
                *status = NativeWindowAcquireStatus::Occluded;
                return true;
            }
            snapshot_.occluded = 0U;
        }
        if (snapshot_.acquired_image_count + snapshot_.in_flight_frame_count >=
            snapshot_.config.limits.maximum_frames_in_flight) {
            *status = NativeWindowAcquireStatus::NotReady;
            return true;
        }
        const std::uint32_t index = swapchain_->GetCurrentBackBufferIndex();
        if (index >= snapshot_.configured_image_count) {
            return fail(error, NativeWindowSwapchainErrorKind::AcquireFailed,
                        "DXGI returned an out-of-range back-buffer index");
        }
        ImageSlot& slot = images_[index];
        if (slot.acquired != 0U || slot.in_flight != 0U) {
            *status = NativeWindowAcquireStatus::NotReady;
            return true;
        }
        slot.acquired = 1U;
        slot.image.acquire_serial = next_acquire_serial_++;
        slot.image.present_serial = 0U;
        slot.image.flags = kNativeWindowSwapchainImageAcquired;
        *image = slot.image;
        *status = NativeWindowAcquireStatus::Acquired;
        snapshot_.acquired_images += 1U;
        snapshot_.acquired_image_count += 1U;
        return true;
    }

    bool present(
        const NativeWindowPresentRequest& request,
        NativeWindowPresentReceipt* receipt,
        NativeWindowSwapchainError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (receipt == nullptr || snapshot_.configured == 0U ||
            request.frame_id == 0U || request.ticket_id == 0U ||
            request.image.image.image.image_index >=
                snapshot_.configured_image_count) {
            return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                        "invalid Direct3D 12 window present request");
        }
        const std::uint32_t index = request.image.image.image.image_index;
        ImageSlot& slot = images_[index];
        if (slot.acquired == 0U ||
            request.image.swapchain_generation !=
                snapshot_.config.swapchain_generation ||
            request.image.acquire_serial != slot.image.acquire_serial ||
            request.image.image.image.device_generation !=
                snapshot_.config.context.device_generation ||
            request.image.image.driver_generation !=
                snapshot_.config.context.runtime_generation ||
            request.image.image.image.surface_id !=
                snapshot_.config.surface.surface_id ||
            request.image.image.image.surface_generation !=
                snapshot_.config.surface.generation_id ||
            request.image.image.image.image_generation !=
                slot.image.image.image.image_generation ||
            request.image.image.native_resource_id !=
                slot.image.image.native_resource_id) {
            snapshot_.stale_rejections += 1U;
            return fail(error, NativeWindowSwapchainErrorKind::StaleGeneration,
                        "Direct3D 12 present references a stale or unowned image");
        }
        if (request.damage_rects.size() >
            snapshot_.config.limits.maximum_damage_rects ||
            request.damage_rects.size() > dirty_rects_.size()) {
            return fail(error, NativeWindowSwapchainErrorKind::ResourceBudgetExceeded,
                        "Direct3D 12 dirty-rectangle count exceeds the configured limit");
        }
        for (const NativeDamageRect& rect : request.damage_rects) {
            if (!damage_rect_valid(rect, snapshot_.config.surface)) {
                return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                            "Direct3D 12 dirty rectangle is outside the surface");
            }
        }
        const bool has_pixel_buffer = !request.pixel_buffer.empty();
        const bool has_shader_surface = !request.shader_surface.empty();
        if (has_pixel_buffer && has_shader_surface) {
            return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                        "Direct3D 12 present cannot use CPU and shader surfaces "
                        "together");
        }
        if (has_pixel_buffer &&
            !native_window_pixel_buffer_valid(request.pixel_buffer, snapshot_.config.surface)) {
            return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                        "Direct3D 12 pixel buffer does not match the surface");
        }
        if (has_shader_surface &&
            (!native_shader_surface_view_valid(request.shader_surface) ||
             request.shader_surface.api_kind != NativeGpuApiKind::Direct3D12 ||
             request.shader_surface.device_generation !=
                 snapshot_.config.context.device_generation ||
             request.shader_surface.runtime_generation !=
                 snapshot_.config.context.runtime_generation ||
             request.shader_surface.width != snapshot_.config.surface.width ||
             request.shader_surface.height != snapshot_.config.surface.height ||
             request.shader_surface.content_checksum != request.command_checksum)) {
            return fail(error, NativeWindowSwapchainErrorKind::StaleGeneration,
                        "Direct3D 12 shader surface is stale or incompatible");
        }
        if ((request.flags & kNativeWindowPresentAllowTearing) != 0U &&
            ((snapshot_.config.flags & kNativeWindowSwapchainAllowTearing) == 0U ||
             snapshot_.config.present_mode != NativePresentMode::Immediate)) {
            return fail(error, NativeWindowSwapchainErrorKind::UnsupportedPresentMode,
                        "DXGI tearing requires enabled immediate presentation");
        }

        *receipt = {};
        receipt->image = request.image;
        receipt->frame_id = request.frame_id;
        receipt->ticket_id = request.ticket_id;
        receipt->wait_fence_value = request.wait_fence_value;
        receipt->command_checksum = request.command_checksum;
        receipt->command_count = request.command_count;
        receipt->damage_rect_count =
            static_cast<std::uint32_t>(request.damage_rects.size());

        if (request.damage_rects.empty() &&
            (request.flags & kNativeWindowPresentFullRedraw) == 0U) {
            release_acquired_locked(slot);
            snapshot_.skipped_frames += 1U;
            receipt->status = NativeWindowPresentStatus::SkippedNoDamage;
            receipt->signal_fence_value = snapshot_.last_submitted_fence_value;
            return true;
        }
        if (snapshot_.in_flight_frame_count >=
            snapshot_.config.limits.maximum_frames_in_flight) {
            return fail(error, NativeWindowSwapchainErrorKind::Backpressure,
                        "Direct3D 12 maximum frames in flight was reached");
        }

        HRESULT result = slot.allocator->Reset();
        if (FAILED(result)) {
            return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                        "ID3D12CommandAllocator::Reset failed", result);
        }
        result = slot.command_list->Reset(slot.allocator.Get(), nullptr);
        if (FAILED(result)) {
            return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                        "ID3D12GraphicsCommandList::Reset failed", result);
        }

        if (has_shader_surface) {
            ID3D12Resource* raw_source = reinterpret_cast<ID3D12Resource*>(
                static_cast<std::uintptr_t>(request.shader_surface.native_resource));
            if (raw_source == nullptr) {
                return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                            "Direct3D 12 shader surface resource is null");
            }
            raw_source->AddRef();
            ComPtr<ID3D12Resource> source;
            source.Attach(raw_source);
            ComPtr<ID3D12Device> source_device;
            result = source->GetDevice(IID_PPV_ARGS(&source_device));
            if (FAILED(result) || source_device.Get() != device_.Get()) {
                return fail(error, NativeWindowSwapchainErrorKind::StaleGeneration,
                            "Direct3D 12 shader surface belongs to another device", result);
            }
            HRESULT resolver_error = S_OK;
            if (!shader_resolver_.encode(slot.command_list.Get(), source.Get(), slot.resource.Get(),
                                         slot.rtv, snapshot_.config.surface.width,
                                         snapshot_.config.surface.height, &resolver_error)) {
                return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                            "Direct3D 12 shader surface resolve failed", resolver_error);
            }
            slot.shader_surface = std::move(source);
        } else if (has_pixel_buffer) {
            const UINT row_pitch = static_cast<UINT>(
                (request.pixel_buffer.row_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1U) &
                ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1U));
            const std::uint64_t upload_bytes =
                static_cast<std::uint64_t>(row_pitch) * request.pixel_buffer.height;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC description{};
            description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            description.Width = upload_bytes;
            description.Height = 1U;
            description.DepthOrArraySize = 1U;
            description.MipLevels = 1U;
            description.SampleDesc.Count = 1U;
            description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> upload;
            result = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                      IID_PPV_ARGS(&upload));
            if (FAILED(result)) {
                return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                            "Direct3D 12 pixel upload allocation failed", result);
            }
            void* mapped = nullptr;
            result = upload->Map(0U, nullptr, &mapped);
            if (FAILED(result) || mapped == nullptr) {
                return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                            "Direct3D 12 pixel upload mapping failed", result);
            }
            auto* destination = static_cast<std::byte*>(mapped);
            for (std::uint32_t row = 0U; row < request.pixel_buffer.height; ++row) {
                std::memcpy(destination + static_cast<std::size_t>(row) * row_pitch,
                            request.pixel_buffer.bytes.data() +
                                static_cast<std::size_t>(row) * request.pixel_buffer.row_bytes,
                            request.pixel_buffer.row_bytes);
            }
            upload->Unmap(0U, nullptr);
            slot.upload = upload;

            D3D12_RESOURCE_BARRIER to_copy{};
            to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            to_copy.Transition.pResource = slot.resource.Get();
            to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            slot.command_list->ResourceBarrier(1U, &to_copy);

            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint.Footprint.Format = map_format(snapshot_.config.surface.format);
            source.PlacedFootprint.Footprint.Width = request.pixel_buffer.width;
            source.PlacedFootprint.Footprint.Height = request.pixel_buffer.height;
            source.PlacedFootprint.Footprint.Depth = 1U;
            source.PlacedFootprint.Footprint.RowPitch = row_pitch;
            D3D12_TEXTURE_COPY_LOCATION destination_location{};
            destination_location.pResource = slot.resource.Get();
            destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination_location.SubresourceIndex = 0U;
            slot.command_list->CopyTextureRegion(&destination_location, 0U, 0U, 0U, &source,
                                                 nullptr);

            D3D12_RESOURCE_BARRIER to_present = to_copy;
            to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            slot.command_list->ResourceBarrier(1U, &to_present);
        } else {
            D3D12_RESOURCE_BARRIER to_render{};
            to_render.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            to_render.Transition.pResource = slot.resource.Get();
            to_render.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            to_render.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            to_render.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            slot.command_list->ResourceBarrier(1U, &to_render);
            const FLOAT color[4] = {
                static_cast<FLOAT>((request.command_checksum >> 0U) & 0xFFU) / 255.0F,
                static_cast<FLOAT>((request.command_checksum >> 8U) & 0xFFU) / 255.0F,
                static_cast<FLOAT>((request.command_checksum >> 16U) & 0xFFU) / 255.0F, 1.0F};
            slot.command_list->ClearRenderTargetView(slot.rtv, color, 0U, nullptr);
            D3D12_RESOURCE_BARRIER to_present = to_render;
            to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            slot.command_list->ResourceBarrier(1U, &to_present);
        }
        result = slot.command_list->Close();
        if (FAILED(result)) {
            return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                        "ID3D12GraphicsCommandList::Close failed", result);
        }
        ID3D12CommandList* command_lists[] = {slot.command_list.Get()};
        queue_->ExecuteCommandLists(1U, command_lists);

        DXGI_PRESENT_PARAMETERS parameters{};
        if ((snapshot_.config.flags & kNativeWindowSwapchainAllowPartialPresent) != 0U &&
            (request.flags & kNativeWindowPresentFullRedraw) == 0U &&
            !request.damage_rects.empty()) {
            for (std::size_t rect_index = 0U;
                 rect_index < request.damage_rects.size();
                 ++rect_index) {
                const NativeDamageRect& rect = request.damage_rects[rect_index];
                RECT& native = dirty_rects_[rect_index];
                native.left = static_cast<LONG>(rect.inline_start);
                native.top = static_cast<LONG>(rect.block_start);
                native.right = static_cast<LONG>(
                    rect.inline_start + static_cast<std::int64_t>(rect.inline_size));
                native.bottom = static_cast<LONG>(
                    rect.block_start + static_cast<std::int64_t>(rect.block_size));
            }
            parameters.DirtyRectsCount =
                static_cast<UINT>(request.damage_rects.size());
            parameters.pDirtyRects = dirty_rects_.data();
        }
        const UINT sync_interval =
            snapshot_.config.present_mode == NativePresentMode::Fifo ? 1U : 0U;
        UINT present_flags = 0U;
        if ((request.flags & kNativeWindowPresentAllowTearing) != 0U) {
            present_flags |= DXGI_PRESENT_ALLOW_TEARING;
        }
        result = swapchain_->Present1(sync_interval, present_flags, &parameters);

        const std::uint64_t signal = next_fence_value_++;
        const HRESULT signal_result = queue_->Signal(fence_.Get(), signal);
        if (FAILED(signal_result)) {
            snapshot_.device_lost = 1U;
            snapshot_.device_lost_events += 1U;
            release_acquired_locked(slot);
            return fail(error, NativeWindowSwapchainErrorKind::DeviceLost,
                        "ID3D12CommandQueue::Signal failed after present",
                        signal_result);
        }

        slot.acquired = 0U;
        slot.in_flight = 1U;
        slot.fence_value = signal;
        slot.image.present_serial = next_present_serial_++;
        slot.image.flags = 0U;
        snapshot_.acquired_image_count -= 1U;
        snapshot_.in_flight_frame_count += 1U;
        const std::uint64_t bytes_per_image =
            snapshot_.current_surface_bytes / snapshot_.configured_image_count;
        snapshot_.current_in_flight_bytes += bytes_per_image;
        snapshot_.peak_in_flight_bytes = std::max(
            snapshot_.peak_in_flight_bytes,
            snapshot_.current_in_flight_bytes);
        snapshot_.last_submitted_fence_value = signal;
        receipt->image = slot.image;
        receipt->signal_fence_value = signal;

        if (result == DXGI_STATUS_OCCLUDED) {
            if (snapshot_.occluded == 0U) {
                snapshot_.occlusion_events += 1U;
            }
            snapshot_.occluded = 1U;
            receipt->status = NativeWindowPresentStatus::Occluded;
            return true;
        }
        if (result == DXGI_ERROR_DEVICE_REMOVED ||
            result == DXGI_ERROR_DEVICE_RESET) {
            if (snapshot_.device_lost == 0U) {
                snapshot_.device_lost_events += 1U;
            }
            snapshot_.device_lost = 1U;
            receipt->status = NativeWindowPresentStatus::DeviceLost;
            return true;
        }
        if (FAILED(result)) {
            return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                        "IDXGISwapChain::Present1 failed", result);
        }
        snapshot_.presented_frames += 1U;
        receipt->status = NativeWindowPresentStatus::Presented;
        return true;
    }

    bool retire_completed(
        std::uint64_t completed_fence_value,
        NativeWindowSwapchainError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (completed_fence_value < snapshot_.completed_fence_value ||
            completed_fence_value > snapshot_.last_submitted_fence_value) {
            return fail(error, NativeWindowSwapchainErrorKind::FenceRegression,
                        "Direct3D 12 completion fence is outside the submitted timeline");
        }
        if (!wait_for_fence_locked(completed_fence_value, error)) {
            return false;
        }
        const std::uint64_t bytes_per_image =
            snapshot_.configured_image_count == 0U
            ? 0U
            : snapshot_.current_surface_bytes /
                snapshot_.configured_image_count;
        for (ImageSlot& slot : images_) {
            if (slot.in_flight != 0U &&
                slot.fence_value <= completed_fence_value) {
                slot.in_flight = 0U;
                slot.fence_value = 0U;
                slot.upload.Reset();
                slot.shader_surface.Reset();
                snapshot_.in_flight_frame_count -= 1U;
                snapshot_.current_in_flight_bytes -= bytes_per_image;
            }
        }
        snapshot_.completed_fence_value = completed_fence_value;
        return true;
    }

    NativeWindowSwapchainSnapshot snapshot() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

    void shutdown() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        NativeWindowSwapchainError ignored;
        (void)wait_for_gpu_locked(&ignored);
        destroy_swapchain_locked();
        shader_resolver_.reset();
        command_queue_context_.Reset();
        device_.Reset();
        factory_.Reset();
        fence_.Reset();
        if (fence_event_ != nullptr) {
            CloseHandle(fence_event_);
            fence_event_ = nullptr;
        }
        const NativeWindowSwapchainCapabilities capabilities_value =
            snapshot_.capabilities;
        const std::uint64_t peak_surface = snapshot_.peak_surface_bytes;
        const std::uint64_t peak_in_flight = snapshot_.peak_in_flight_bytes;
        snapshot_ = {};
        snapshot_.capabilities = capabilities_value;
        snapshot_.peak_surface_bytes = peak_surface;
        snapshot_.peak_in_flight_bytes = peak_in_flight;
    }

private:
    struct ImageSlot final {
        ComPtr<ID3D12Resource> resource;
        ComPtr<ID3D12Resource> upload;
        ComPtr<ID3D12Resource> shader_surface;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> command_list;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        NativeWindowSwapchainImage image;
        std::uint64_t fence_value{0};
        std::uint8_t acquired{0};
        std::uint8_t in_flight{0};
        std::uint8_t reserved[6]{0, 0, 0, 0, 0, 0};
    };

    bool configure_locked(
        const NativeWindowSwapchainConfig& config,
        bool recreation,
        NativeWindowSwapchainError* error) noexcept {
        const NativeWindowSwapchainCapabilities capabilities_value =
            snapshot_.capabilities;
        const std::uint32_t required_context_flags =
            kNativeGpuSdkContextDeviceValid |
            kNativeGpuSdkContextGraphicsQueueValid |
            kNativeGpuSdkContextPresentQueueValid;
        if (config.context.api_kind != NativeGpuApiKind::Direct3D12 ||
            (config.context.flags & required_context_flags) !=
                required_context_flags ||
            config.context.device_generation == 0U ||
            config.context.runtime_generation == 0U ||
            config.context.instance_or_factory == 0U ||
            config.context.device == 0U ||
            config.context.graphics_queue == 0U ||
            config.context.present_queue != config.context.graphics_queue) {
            return fail(error, NativeWindowSwapchainErrorKind::NativeContextUnavailable,
                        "Direct3D 12 native context handoff is incomplete or stale");
        }
        if (config.window.system != NativeWindowSystem::Win32 ||
            config.window.generation == 0U ||
            config.window.window_or_layer == 0U ||
            config.surface.surface_id == 0U ||
            config.surface.generation_id == 0U ||
            config.surface.width == 0U || config.surface.height == 0U ||
            config.swapchain_generation == 0U || config.image_count < 2U ||
            config.image_count > images_.size()) {
            return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                        "invalid Direct3D 12 window swapchain configuration");
        }
        if (config.present_mode == NativePresentMode::Mailbox) {
            return fail(error, NativeWindowSwapchainErrorKind::UnsupportedPresentMode,
                        "DXGI flip-model swapchains do not expose mailbox mode");
        }
        if (config.image_count > config.limits.maximum_image_count ||
            config.image_count > capabilities_value.maximum_image_count ||
            config.limits.maximum_frames_in_flight == 0U ||
            config.limits.maximum_frames_in_flight > config.image_count ||
            config.limits.maximum_frames_in_flight >
                capabilities_value.maximum_frames_in_flight ||
            config.limits.maximum_damage_rects == 0U ||
            config.limits.maximum_damage_rects >
                capabilities_value.maximum_damage_rects ||
            config.surface.width > config.limits.maximum_width ||
            config.surface.height > config.limits.maximum_height ||
            config.surface.width > capabilities_value.maximum_width ||
            config.surface.height > capabilities_value.maximum_height) {
            return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                        "Direct3D 12 window limits exceed the certified envelope");
        }
        std::uint64_t bytes_per_image = 0U;
        std::uint64_t total_bytes = 0U;
        if (!checked_surface_bytes(
                config.surface,
                config.image_count,
                &bytes_per_image,
                &total_bytes)) {
            return fail(error, NativeWindowSwapchainErrorKind::ArithmeticOverflow,
                        "Direct3D 12 window surface byte count overflowed");
        }
        if (total_bytes > config.limits.maximum_surface_bytes ||
            total_bytes > capabilities_value.maximum_surface_bytes ||
            bytes_per_image >
                config.limits.maximum_in_flight_bytes /
                    config.limits.maximum_frames_in_flight) {
            return fail(error, NativeWindowSwapchainErrorKind::ResourceBudgetExceeded,
                        "Direct3D 12 window surface exceeds the configured budget");
        }
        if (recreation) {
            if (snapshot_.acquired_image_count != 0U ||
                snapshot_.in_flight_frame_count != 0U) {
                return fail(error, NativeWindowSwapchainErrorKind::Backpressure,
                            "Direct3D 12 swapchain cannot resize while images are owned");
            }
            if (config.swapchain_generation <=
                    snapshot_.config.swapchain_generation ||
                config.context.device_generation !=
                    snapshot_.config.context.device_generation ||
                config.context.runtime_generation !=
                    snapshot_.config.context.runtime_generation ||
                config.window.generation != snapshot_.config.window.generation ||
                !(config.surface == snapshot_.pending_surface)) {
                snapshot_.stale_rejections += 1U;
                return fail(error, NativeWindowSwapchainErrorKind::StaleGeneration,
                            "Direct3D 12 recreation references stale generations");
            }
        }

        ComPtr<IDXGIFactory6> new_factory =
            retain_com_pointer<IDXGIFactory6>(config.context.instance_or_factory);
        ComPtr<ID3D12Device> new_device =
            retain_com_pointer<ID3D12Device>(config.context.device);
        ComPtr<ID3D12CommandQueue> new_queue =
            retain_com_pointer<ID3D12CommandQueue>(config.context.graphics_queue);
        if (new_factory == nullptr || new_device == nullptr || new_queue == nullptr) {
            return fail(error, NativeWindowSwapchainErrorKind::NativeContextUnavailable,
                        "Direct3D 12 context pointers could not be retained");
        }

        BOOL tearing_supported = FALSE;
        const HRESULT tearing_result = new_factory->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &tearing_supported,
            sizeof(tearing_supported));
        const bool allow_tearing =
            (config.flags & kNativeWindowSwapchainAllowTearing) != 0U;
        if (allow_tearing &&
            (FAILED(tearing_result) || tearing_supported == FALSE)) {
            return fail(error, NativeWindowSwapchainErrorKind::UnsupportedPresentMode,
                        "DXGI tearing was requested but is unavailable",
                        tearing_result);
        }

        if (fence_ == nullptr) {
            const HRESULT fence_result =
                new_device->CreateFence(0U, D3D12_FENCE_FLAG_NONE,
                                       IID_PPV_ARGS(&fence_));
            if (FAILED(fence_result)) {
                return fail(error, NativeWindowSwapchainErrorKind::SwapchainCreationFailed,
                            "Direct3D 12 window fence creation failed",
                            fence_result);
            }
            fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (fence_event_ == nullptr) {
                return fail(error, NativeWindowSwapchainErrorKind::SwapchainCreationFailed,
                            "Direct3D 12 window fence event creation failed",
                            HRESULT_FROM_WIN32(GetLastError()));
            }
        }

        const HWND window = reinterpret_cast<HWND>(
            static_cast<std::uintptr_t>(config.window.window_or_layer));
        if (!IsWindow(window)) {
            return fail(error, NativeWindowSwapchainErrorKind::SurfaceCreationFailed,
                        "Direct3D 12 window handle is not a live HWND");
        }

        if (!wait_for_gpu_locked(error)) {
            return false;
        }
        release_image_resources_locked();

        const UINT swapchain_flags = allow_tearing
            ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
            : 0U;
        HRESULT result = S_OK;
        if (!recreation || swapchain_ == nullptr) {
            DXGI_SWAP_CHAIN_DESC1 description{};
            description.Width = config.surface.width;
            description.Height = config.surface.height;
            description.Format = map_format(config.surface.format);
            description.Stereo = FALSE;
            description.SampleDesc.Count = 1U;
            description.SampleDesc.Quality = 0U;
            description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            description.BufferCount = config.image_count;
            description.Scaling = DXGI_SCALING_STRETCH;
            description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            description.Flags = swapchain_flags;
            ComPtr<IDXGISwapChain1> swapchain1;
            result = new_factory->CreateSwapChainForHwnd(
                new_queue.Get(), window, &description, nullptr, nullptr,
                &swapchain1);
            if (FAILED(result)) {
                return fail(error, NativeWindowSwapchainErrorKind::SwapchainCreationFailed,
                            "IDXGIFactory::CreateSwapChainForHwnd failed", result);
            }
            result = swapchain1.As(&swapchain_);
            if (FAILED(result)) {
                swapchain_.Reset();
                return fail(error, NativeWindowSwapchainErrorKind::SwapchainCreationFailed,
                            "DXGI swapchain does not expose IDXGISwapChain3", result);
            }
            (void)new_factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        } else {
            result = swapchain_->ResizeBuffers(
                config.image_count,
                config.surface.width,
                config.surface.height,
                map_format(config.surface.format),
                swapchain_flags);
            if (FAILED(result)) {
                return fail(error, NativeWindowSwapchainErrorKind::SwapchainCreationFailed,
                            "IDXGISwapChain::ResizeBuffers failed", result);
            }
        }

        D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
        heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_description.NumDescriptors = config.image_count;
        heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ComPtr<ID3D12DescriptorHeap> new_rtv_heap;
        result = new_device->CreateDescriptorHeap(
            &heap_description, IID_PPV_ARGS(&new_rtv_heap));
        if (FAILED(result)) {
            return fail(error, NativeWindowSwapchainErrorKind::SwapchainCreationFailed,
                        "Direct3D 12 window RTV heap creation failed", result);
        }
        const UINT rtv_increment = new_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            new_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (std::uint32_t index = 0U; index < config.image_count; ++index) {
            ImageSlot& slot = images_[index];
            result = swapchain_->GetBuffer(index, IID_PPV_ARGS(&slot.resource));
            if (FAILED(result)) {
                release_image_resources_locked();
                return fail(error, NativeWindowSwapchainErrorKind::SwapchainCreationFailed,
                            "IDXGISwapChain::GetBuffer failed", result);
            }
            result = new_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&slot.allocator));
            if (FAILED(result)) {
                release_image_resources_locked();
                return fail(error, NativeWindowSwapchainErrorKind::SwapchainCreationFailed,
                            "Direct3D 12 window command allocator creation failed",
                            result);
            }
            result = new_device->CreateCommandList(
                0U,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                slot.allocator.Get(),
                nullptr,
                IID_PPV_ARGS(&slot.command_list));
            if (FAILED(result)) {
                release_image_resources_locked();
                return fail(error, NativeWindowSwapchainErrorKind::SwapchainCreationFailed,
                            "Direct3D 12 window command list creation failed", result);
            }
            result = slot.command_list->Close();
            if (FAILED(result)) {
                release_image_resources_locked();
                return fail(error, NativeWindowSwapchainErrorKind::SwapchainCreationFailed,
                            "Direct3D 12 initial window command list close failed",
                            result);
            }
            new_device->CreateRenderTargetView(slot.resource.Get(), nullptr, rtv);
            slot.rtv = rtv;
            slot.image.image.image.device_generation =
                config.context.device_generation;
            slot.image.image.image.surface_id = config.surface.surface_id;
            slot.image.image.image.surface_generation =
                config.surface.generation_id;
            slot.image.image.image.image_generation = next_image_generation_++;
            slot.image.image.image.image_index = index;
            slot.image.image.image.flags = 0U;
            slot.image.image.driver_generation = config.context.runtime_generation;
            slot.image.image.native_resource_id = next_resource_id_++;
            slot.image.image.state = NativePlatformResourceState::Present;
            slot.image.swapchain_generation = config.swapchain_generation;
            rtv.ptr += rtv_increment;
        }

        HRESULT resolver_result = S_OK;
        if (!shader_resolver_.configure(new_device.Get(), map_format(config.surface.format),
                                        &resolver_result)) {
            release_image_resources_locked();
            return fail(error, NativeWindowSwapchainErrorKind::SwapchainCreationFailed,
                        "Direct3D 12 shader resolve pipeline creation failed", resolver_result);
        }

        factory_ = std::move(new_factory);
        device_ = std::move(new_device);
        command_queue_context_ = std::move(new_queue);
        queue_ = command_queue_context_.Get();
        rtv_heap_ = std::move(new_rtv_heap);
        rtv_increment_ = rtv_increment;
        snapshot_.config = config;
        snapshot_.pending_surface = {};
        snapshot_.configured_image_count = config.image_count;
        snapshot_.current_surface_bytes = total_bytes;
        snapshot_.peak_surface_bytes = std::max(
            snapshot_.peak_surface_bytes, total_bytes);
        snapshot_.current_in_flight_bytes = 0U;
        snapshot_.configured = 1U;
        snapshot_.out_of_date = 0U;
        snapshot_.occluded = 0U;
        snapshot_.device_lost = 0U;
        if (recreation) {
            snapshot_.recreations += 1U;
        } else {
            snapshot_.configurations += 1U;
        }
        return true;
    }

    bool wait_for_fence_locked(
        std::uint64_t value,
        NativeWindowSwapchainError* error) noexcept {
        if (value == 0U || fence_ == nullptr) {
            return true;
        }
        if (fence_->GetCompletedValue() >= value) {
            return true;
        }
        const HRESULT result = fence_->SetEventOnCompletion(value, fence_event_);
        if (FAILED(result)) {
            return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                        "ID3D12Fence::SetEventOnCompletion failed", result);
        }
        const DWORD wait_result =
            WaitForSingleObject(fence_event_, kFenceTimeoutMs);
        if (wait_result != WAIT_OBJECT_0) {
            return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                        "Direct3D 12 window fence wait timed out",
                        HRESULT_FROM_WIN32(
                            wait_result == WAIT_FAILED
                            ? GetLastError()
                            : ERROR_TIMEOUT));
        }
        return true;
    }

    bool wait_for_gpu_locked(NativeWindowSwapchainError* error) noexcept {
        if (queue_ == nullptr || fence_ == nullptr) {
            return true;
        }
        const std::uint64_t signal = next_fence_value_++;
        const HRESULT result = queue_->Signal(fence_.Get(), signal);
        if (FAILED(result)) {
            return fail(error, NativeWindowSwapchainErrorKind::DeviceLost,
                        "Direct3D 12 window idle signal failed", result);
        }
        return wait_for_fence_locked(signal, error);
    }

    void release_acquired_locked(ImageSlot& slot) noexcept {
        slot.acquired = 0U;
        slot.image.flags = 0U;
        if (snapshot_.acquired_image_count != 0U) {
            snapshot_.acquired_image_count -= 1U;
        }
    }

    void release_image_resources_locked() noexcept {
        for (ImageSlot& slot : images_) {
            slot.resource.Reset();
            slot.upload.Reset();
            slot.shader_surface.Reset();
            slot.allocator.Reset();
            slot.command_list.Reset();
            slot.rtv = {};
            slot.image = {};
            slot.fence_value = 0U;
            slot.acquired = 0U;
            slot.in_flight = 0U;
        }
        rtv_heap_.Reset();
        rtv_increment_ = 0U;
        snapshot_.configured_image_count = 0U;
        snapshot_.acquired_image_count = 0U;
        snapshot_.in_flight_frame_count = 0U;
        snapshot_.current_surface_bytes = 0U;
        snapshot_.current_in_flight_bytes = 0U;
    }

    void destroy_swapchain_locked() noexcept {
        release_image_resources_locked();
        swapchain_.Reset();
        queue_ = nullptr;
    }

    mutable std::mutex mutex_;
    NativeWindowSwapchainSnapshot snapshot_;
    ComPtr<IDXGIFactory6> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> command_queue_context_;
    ID3D12CommandQueue* queue_{nullptr};
    ComPtr<IDXGISwapChain3> swapchain_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    detail::D3D12ShaderSurfaceResolver shader_resolver_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_{nullptr};
    std::array<ImageSlot, 16U> images_{};
    std::array<RECT, 64U> dirty_rects_{};
    UINT rtv_increment_{0U};
    std::uint64_t next_image_generation_{1U};
    std::uint64_t next_resource_id_{1U};
    std::uint64_t next_acquire_serial_{1U};
    std::uint64_t next_present_serial_{1U};
    std::uint64_t next_fence_value_{1U};
};

} // namespace

std::unique_ptr<NativeWindowSwapchainApi>
make_direct3d12_native_window_swapchain_api() noexcept {
    try {
        return std::make_unique<Direct3D12NativeWindowSwapchainApi>();
    } catch (...) {
        return nullptr;
    }
}

bool native_window_swapchain_build_has_backend(
    NativeGpuApiKind kind,
    NativeWindowSystem system) noexcept {
    return kind == NativeGpuApiKind::Direct3D12 &&
        system == NativeWindowSystem::Win32;
}

} // namespace zevryon::text

#endif // ZEVRYON_HAS_D3D12_WINDOW_SWAPCHAIN
