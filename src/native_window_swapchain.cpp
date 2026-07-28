#include "native_window_swapchain.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace zevryon::text {
namespace {

constexpr std::uint64_t kBytesPerPixel = 4U;

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
    std::int64_t native_code = 0) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->native_code = native_code;
        try {
            error->message = message != nullptr ? message : "native window swapchain failure";
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool api_kind_supported(NativeGpuApiKind kind) noexcept {
    return kind == NativeGpuApiKind::Vulkan ||
        kind == NativeGpuApiKind::Metal ||
        kind == NativeGpuApiKind::Direct3D12;
}

bool window_system_supported(
    NativeGpuApiKind kind,
    NativeWindowSystem system) noexcept {
    switch (kind) {
        case NativeGpuApiKind::Vulkan:
            return system == NativeWindowSystem::Win32 ||
                system == NativeWindowSystem::Xcb ||
                system == NativeWindowSystem::Wayland;
        case NativeGpuApiKind::Metal:
            return system == NativeWindowSystem::CocoaLayer;
        case NativeGpuApiKind::Direct3D12:
            return system == NativeWindowSystem::Win32;
        case NativeGpuApiKind::ReferenceCpu:
            break;
    }
    return false;
}

bool present_mode_supported(
    NativePresentMode mode,
    const NativeWindowSwapchainCapabilities& capabilities,
    std::uint32_t flags) noexcept {
    switch (mode) {
        case NativePresentMode::Fifo:
            return true;
        case NativePresentMode::Mailbox:
            return (capabilities.flags & kNativeWindowSwapchainMailbox) != 0U &&
                (flags & kNativeWindowSwapchainAllowMailbox) != 0U;
        case NativePresentMode::Immediate:
            return (capabilities.flags & kNativeWindowSwapchainImmediate) != 0U &&
                (flags & kNativeWindowSwapchainAllowImmediate) != 0U;
    }
    return false;
}

bool checked_surface_bytes(
    const GpuSurfaceDescriptor& surface,
    std::uint64_t image_count,
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
    if (*bytes_per_image > std::numeric_limits<std::uint64_t>::max() / image_count) {
        return false;
    }
    *total_bytes = *bytes_per_image * image_count;
    return true;
}

bool context_valid(
    const NativeGpuSdkContextHandle& context,
    bool require_native) noexcept {
    if (!api_kind_supported(context.api_kind) ||
        context.device_generation == 0U ||
        context.runtime_generation == 0U) {
        return false;
    }
    if (!require_native) {
        return true;
    }
    const std::uint32_t required =
        kNativeGpuSdkContextDeviceValid |
        kNativeGpuSdkContextGraphicsQueueValid |
        kNativeGpuSdkContextPresentQueueValid;
    if ((context.flags & required) != required ||
        context.device == 0U ||
        context.graphics_queue == 0U ||
        context.present_queue == 0U) {
        return false;
    }
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
    if (x > surface.width || y > surface.height ||
        rect.inline_size > static_cast<std::uint64_t>(surface.width) - x ||
        rect.block_size > static_cast<std::uint64_t>(surface.height) - y) {
        return false;
    }
    return true;
}

} // namespace

const char* native_window_swapchain_error_kind_name(
    NativeWindowSwapchainErrorKind kind) noexcept {
    switch (kind) {
        case NativeWindowSwapchainErrorKind::None: return "none";
        case NativeWindowSwapchainErrorKind::InvalidInput: return "invalid_input";
        case NativeWindowSwapchainErrorKind::UnsupportedWindowSystem:
            return "unsupported_window_system";
        case NativeWindowSwapchainErrorKind::UnsupportedPresentMode:
            return "unsupported_present_mode";
        case NativeWindowSwapchainErrorKind::NativeContextUnavailable:
            return "native_context_unavailable";
        case NativeWindowSwapchainErrorKind::SurfaceCreationFailed:
            return "surface_creation_failed";
        case NativeWindowSwapchainErrorKind::SwapchainCreationFailed:
            return "swapchain_creation_failed";
        case NativeWindowSwapchainErrorKind::ResourceBudgetExceeded:
            return "resource_budget_exceeded";
        case NativeWindowSwapchainErrorKind::AcquireFailed: return "acquire_failed";
        case NativeWindowSwapchainErrorKind::PresentFailed: return "present_failed";
        case NativeWindowSwapchainErrorKind::Backpressure: return "backpressure";
        case NativeWindowSwapchainErrorKind::OutOfDate: return "out_of_date";
        case NativeWindowSwapchainErrorKind::Occluded: return "occluded";
        case NativeWindowSwapchainErrorKind::DeviceLost: return "device_lost";
        case NativeWindowSwapchainErrorKind::FenceRegression:
            return "fence_regression";
        case NativeWindowSwapchainErrorKind::StaleGeneration:
            return "stale_generation";
        case NativeWindowSwapchainErrorKind::ArithmeticOverflow:
            return "arithmetic_overflow";
    }
    return "unknown";
}

ReferenceNativeWindowSwapchainApi::ReferenceNativeWindowSwapchainApi(
    NativeWindowSwapchainCapabilities capabilities) noexcept {
    snapshot_.capabilities = capabilities;
}

NativeWindowSwapchainCapabilities
ReferenceNativeWindowSwapchainApi::capabilities() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_.capabilities;
}

bool ReferenceNativeWindowSwapchainApi::configure(
    const NativeWindowSwapchainConfig& config,
    NativeWindowSwapchainError* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_error(error);
    if (snapshot_.configured != 0U) {
        return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                    "window swapchain is already configured");
    }
    return configure_locked(config, false, error);
}

bool ReferenceNativeWindowSwapchainApi::configure_locked(
    const NativeWindowSwapchainConfig& config,
    bool recreation,
    NativeWindowSwapchainError* error) noexcept {
    const NativeWindowSwapchainCapabilities capabilities_value =
        snapshot_.capabilities;
    const bool require_native =
        (config.flags & kNativeWindowSwapchainRequireNativeContext) != 0U;
    if (!context_valid(config.context, require_native)) {
        return fail(error, NativeWindowSwapchainErrorKind::NativeContextUnavailable,
                    "native GPU context handoff is incomplete or stale");
    }
    if (!window_system_supported(config.context.api_kind, config.window.system)) {
        return fail(error, NativeWindowSwapchainErrorKind::UnsupportedWindowSystem,
                    "window system is not compatible with the selected GPU API");
    }
    if (config.window.generation == 0U ||
        config.window.window_or_layer == 0U ||
        config.surface.surface_id == 0U ||
        config.surface.generation_id == 0U ||
        config.surface.width == 0U ||
        config.surface.height == 0U ||
        config.swapchain_generation == 0U ||
        config.image_count == 0U) {
        return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                    "window swapchain configuration contains zero identifiers or extent");
    }
    if (config.surface.width > config.limits.maximum_width ||
        config.surface.height > config.limits.maximum_height ||
        config.surface.width > capabilities_value.maximum_width ||
        config.surface.height > capabilities_value.maximum_height ||
        config.image_count < capabilities_value.minimum_image_count ||
        config.image_count > config.limits.maximum_image_count ||
        config.image_count > capabilities_value.maximum_image_count ||
        config.image_count > images_.size() ||
        config.limits.maximum_frames_in_flight == 0U ||
        config.limits.maximum_frames_in_flight > config.image_count ||
        config.limits.maximum_frames_in_flight >
            capabilities_value.maximum_frames_in_flight ||
        config.limits.maximum_damage_rects == 0U ||
        config.limits.maximum_damage_rects >
            capabilities_value.maximum_damage_rects) {
        return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                    "window swapchain limits exceed the certified capability envelope");
    }
    if (!present_mode_supported(
            config.present_mode, capabilities_value, config.flags)) {
        return fail(error, NativeWindowSwapchainErrorKind::UnsupportedPresentMode,
                    "requested present mode is not enabled or supported");
    }
    if ((config.flags & kNativeWindowSwapchainAllowTearing) != 0U &&
        (capabilities_value.flags & kNativeWindowSwapchainTearing) == 0U) {
        return fail(error, NativeWindowSwapchainErrorKind::UnsupportedPresentMode,
                    "tearing was requested but the backend does not support it");
    }
    if ((config.flags & kNativeWindowSwapchainAllowPartialPresent) != 0U &&
        (capabilities_value.flags & kNativeWindowSwapchainPartialPresent) == 0U) {
        return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                    "partial present was requested but is unsupported");
    }
    std::uint64_t bytes_per_image = 0U;
    std::uint64_t total_bytes = 0U;
    if (!checked_surface_bytes(
            config.surface, config.image_count, &bytes_per_image, &total_bytes)) {
        return fail(error, NativeWindowSwapchainErrorKind::ArithmeticOverflow,
                    "window swapchain surface byte count overflowed");
    }
    if (total_bytes > config.limits.maximum_surface_bytes ||
        total_bytes > capabilities_value.maximum_surface_bytes ||
        bytes_per_image >
            config.limits.maximum_in_flight_bytes /
                config.limits.maximum_frames_in_flight) {
        return fail(error, NativeWindowSwapchainErrorKind::ResourceBudgetExceeded,
                    "window swapchain image or in-flight byte budget was exceeded");
    }
    if (recreation) {
        if (snapshot_.acquired_image_count != 0U ||
            snapshot_.in_flight_frame_count != 0U) {
            return fail(error, NativeWindowSwapchainErrorKind::Backpressure,
                        "window swapchain cannot be recreated while images are owned");
        }
        if (config.swapchain_generation <= snapshot_.config.swapchain_generation ||
            config.context.device_generation !=
                snapshot_.config.context.device_generation ||
            config.context.runtime_generation !=
                snapshot_.config.context.runtime_generation ||
            config.window.generation != snapshot_.config.window.generation ||
            !(config.surface == snapshot_.pending_surface)) {
            snapshot_.stale_rejections += 1U;
            return fail(error, NativeWindowSwapchainErrorKind::StaleGeneration,
                        "window swapchain recreation references stale generations");
        }
    }

    reset_images_locked();
    snapshot_.config = config;
    snapshot_.pending_surface = {};
    snapshot_.configured_image_count = config.image_count;
    snapshot_.current_surface_bytes = total_bytes;
    snapshot_.peak_surface_bytes =
        std::max(snapshot_.peak_surface_bytes, total_bytes);
    snapshot_.current_in_flight_bytes = 0U;
    snapshot_.configured = 1U;
    snapshot_.out_of_date = 0U;
    snapshot_.occluded = 0U;
    snapshot_.device_lost = 0U;
    next_image_index_ = 0U;
    for (std::uint32_t index = 0U; index < config.image_count; ++index) {
        ImageSlot& slot = images_[index];
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
    }
    if (recreation) {
        snapshot_.recreations += 1U;
    } else {
        snapshot_.configurations += 1U;
    }
    return true;
}

bool ReferenceNativeWindowSwapchainApi::request_resize(
    const GpuSurfaceDescriptor& surface,
    NativeWindowSwapchainError* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_error(error);
    if (snapshot_.configured == 0U ||
        surface.surface_id != snapshot_.config.surface.surface_id ||
        surface.generation_id <= snapshot_.config.surface.generation_id ||
        surface.width == 0U ||
        surface.height == 0U ||
        surface.width > snapshot_.config.limits.maximum_width ||
        surface.height > snapshot_.config.limits.maximum_height ||
        surface.width > snapshot_.capabilities.maximum_width ||
        surface.height > snapshot_.capabilities.maximum_height) {
        snapshot_.stale_rejections += 1U;
        return fail(error, NativeWindowSwapchainErrorKind::StaleGeneration,
                    "resize request is invalid or does not advance surface generation");
    }
    std::uint64_t bytes_per_image = 0U;
    std::uint64_t total_bytes = 0U;
    if (!checked_surface_bytes(
            surface,
            snapshot_.config.image_count,
            &bytes_per_image,
            &total_bytes)) {
        return fail(error, NativeWindowSwapchainErrorKind::ArithmeticOverflow,
                    "resized surface byte count overflowed");
    }
    if (total_bytes > snapshot_.config.limits.maximum_surface_bytes ||
        total_bytes > snapshot_.capabilities.maximum_surface_bytes ||
        bytes_per_image >
            snapshot_.config.limits.maximum_in_flight_bytes /
                snapshot_.config.limits.maximum_frames_in_flight) {
        return fail(error, NativeWindowSwapchainErrorKind::ResourceBudgetExceeded,
                    "resized surface exceeds the certified byte budget");
    }
    snapshot_.pending_surface = surface;
    snapshot_.out_of_date = 1U;
    snapshot_.resize_requests += 1U;
    snapshot_.out_of_date_events += 1U;
    return true;
}

bool ReferenceNativeWindowSwapchainApi::recreate(
    const NativeWindowSwapchainConfig& config,
    NativeWindowSwapchainError* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_error(error);
    if (snapshot_.configured == 0U ||
        snapshot_.out_of_date == 0U ||
        snapshot_.pending_surface.surface_id == 0U) {
        return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                    "window swapchain recreation was not requested");
    }
    return configure_locked(config, true, error);
}

bool ReferenceNativeWindowSwapchainApi::acquire(
    std::uint64_t ticket_id,
    NativeWindowSwapchainImage* image,
    NativeWindowAcquireStatus* status,
    NativeWindowSwapchainError* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_error(error);
    if (image == nullptr || status == nullptr || ticket_id == 0U ||
        snapshot_.configured == 0U) {
        return fail(error, NativeWindowSwapchainErrorKind::AcquireFailed,
                    "invalid window swapchain acquire request");
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
        *status = NativeWindowAcquireStatus::Occluded;
        return true;
    }

    if (snapshot_.acquired_image_count +
            snapshot_.in_flight_frame_count >=
        snapshot_.config.limits.maximum_frames_in_flight) {
        *status = NativeWindowAcquireStatus::NotReady;
        return true;
    }

    const NativeWindowAcquireStatus requested =
        std::exchange(next_acquire_status_, NativeWindowAcquireStatus::Acquired);
    if (requested != NativeWindowAcquireStatus::Acquired &&
        requested != NativeWindowAcquireStatus::Suboptimal) {
        *status = requested;
        if (requested == NativeWindowAcquireStatus::OutOfDate) {
            snapshot_.out_of_date = 1U;
            snapshot_.out_of_date_events += 1U;
        } else if (requested == NativeWindowAcquireStatus::Occluded) {
            snapshot_.occluded = 1U;
            snapshot_.occlusion_events += 1U;
        } else if (requested == NativeWindowAcquireStatus::DeviceLost) {
            snapshot_.device_lost = 1U;
            snapshot_.device_lost_events += 1U;
        }
        return true;
    }

    for (std::uint32_t offset = 0U;
         offset < snapshot_.configured_image_count;
         ++offset) {
        const std::uint32_t index =
            (next_image_index_ + offset) % snapshot_.configured_image_count;
        ImageSlot& slot = images_[index];
        if (slot.acquired == 0U && slot.in_flight == 0U) {
            next_image_index_ =
                (index + 1U) % snapshot_.configured_image_count;
            slot.acquired = 1U;
            slot.image.acquire_serial = next_acquire_serial_++;
            slot.image.present_serial = 0U;
            slot.image.flags = kNativeWindowSwapchainImageAcquired;
            if (requested == NativeWindowAcquireStatus::Suboptimal) {
                slot.image.flags |= kNativeWindowSwapchainImageSuboptimal;
            }
            *image = slot.image;
            *status = requested;
            snapshot_.acquired_images += 1U;
            snapshot_.acquired_image_count += 1U;
            return true;
        }
    }
    *status = NativeWindowAcquireStatus::NotReady;
    return true;
}

bool ReferenceNativeWindowSwapchainApi::present(
    const NativeWindowPresentRequest& request,
    NativeWindowPresentReceipt* receipt,
    NativeWindowSwapchainError* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_error(error);
    if (receipt == nullptr || snapshot_.configured == 0U ||
        request.frame_id == 0U || request.ticket_id == 0U ||
        request.image.image.image.image_index >=
            snapshot_.configured_image_count) {
        return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                    "invalid window swapchain present request");
    }
    const std::uint32_t image_index =
        request.image.image.image.image_index;
    ImageSlot& slot = images_[image_index];
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
                    "present request references a stale or unowned image");
    }
    if (request.damage_rects.size() >
        snapshot_.config.limits.maximum_damage_rects) {
        return fail(error, NativeWindowSwapchainErrorKind::ResourceBudgetExceeded,
                    "present damage rectangle count exceeds the configured limit");
    }
    for (const NativeDamageRect& rect : request.damage_rects) {
        if (!damage_rect_valid(rect, snapshot_.config.surface)) {
            return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                        "present damage rectangle is outside the surface");
        }
    }
    if (!native_window_pixel_buffer_valid(
            request.pixel_buffer, snapshot_.config.surface)) {
        return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                    "present pixel buffer does not match the configured surface");
    }
    if ((request.flags & kNativeWindowPresentAllowTearing) != 0U &&
        ((snapshot_.config.flags & kNativeWindowSwapchainAllowTearing) == 0U ||
         snapshot_.config.present_mode != NativePresentMode::Immediate)) {
        return fail(error, NativeWindowSwapchainErrorKind::UnsupportedPresentMode,
                    "tearing is valid only for an enabled immediate present mode");
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
        slot.acquired = 0U;
        slot.image.flags = 0U;
        snapshot_.acquired_image_count -= 1U;
        snapshot_.skipped_frames += 1U;
        receipt->status = NativeWindowPresentStatus::SkippedNoDamage;
        receipt->signal_fence_value =
            snapshot_.last_submitted_fence_value;
        return true;
    }
    if (snapshot_.in_flight_frame_count >=
        snapshot_.config.limits.maximum_frames_in_flight) {
        return fail(error, NativeWindowSwapchainErrorKind::Backpressure,
                    "maximum frames in flight was reached");
    }

    NativeWindowPresentStatus requested =
        std::exchange(next_present_status_, NativeWindowPresentStatus::Presented);
    if (snapshot_.device_lost != 0U) {
        requested = NativeWindowPresentStatus::DeviceLost;
    } else if (snapshot_.out_of_date != 0U) {
        requested = NativeWindowPresentStatus::OutOfDate;
    } else if (snapshot_.occluded != 0U) {
        requested = NativeWindowPresentStatus::Occluded;
    }
    if (requested == NativeWindowPresentStatus::OutOfDate ||
        requested == NativeWindowPresentStatus::Occluded ||
        requested == NativeWindowPresentStatus::DeviceLost) {
        slot.acquired = 0U;
        slot.image.flags = 0U;
        snapshot_.acquired_image_count -= 1U;
        receipt->status = requested;
        receipt->signal_fence_value =
            snapshot_.last_submitted_fence_value;
        if (requested == NativeWindowPresentStatus::OutOfDate) {
            if (snapshot_.out_of_date == 0U) {
                snapshot_.out_of_date_events += 1U;
            }
            snapshot_.out_of_date = 1U;
        } else if (requested == NativeWindowPresentStatus::Occluded) {
            if (snapshot_.occluded == 0U) {
                snapshot_.occlusion_events += 1U;
            }
            snapshot_.occluded = 1U;
        } else {
            if (snapshot_.device_lost == 0U) {
                snapshot_.device_lost_events += 1U;
            }
            snapshot_.device_lost = 1U;
        }
        return true;
    }

    const std::uint64_t bytes_per_image =
        snapshot_.current_surface_bytes /
        snapshot_.configured_image_count;
    if (snapshot_.current_in_flight_bytes >
        snapshot_.config.limits.maximum_in_flight_bytes -
            bytes_per_image) {
        return fail(error, NativeWindowSwapchainErrorKind::ResourceBudgetExceeded,
                    "in-flight window surface byte budget was exceeded");
    }
    const std::uint64_t signal = next_fence_value_++;
    slot.acquired = 0U;
    slot.in_flight = 1U;
    slot.fence_value = signal;
    slot.image.present_serial = next_present_serial_++;
    slot.image.flags = 0U;
    snapshot_.acquired_image_count -= 1U;
    snapshot_.in_flight_frame_count += 1U;
    snapshot_.current_in_flight_bytes += bytes_per_image;
    snapshot_.peak_in_flight_bytes = std::max(
        snapshot_.peak_in_flight_bytes,
        snapshot_.current_in_flight_bytes);
    snapshot_.presented_frames += 1U;
    snapshot_.last_submitted_fence_value = signal;

    receipt->image = slot.image;
    receipt->status = requested;
    receipt->signal_fence_value = signal;
    return true;
}

bool ReferenceNativeWindowSwapchainApi::retire_completed(
    std::uint64_t completed_fence_value,
    NativeWindowSwapchainError* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_error(error);
    if (completed_fence_value < snapshot_.completed_fence_value ||
        completed_fence_value > snapshot_.last_submitted_fence_value) {
        return fail(error, NativeWindowSwapchainErrorKind::FenceRegression,
                    "completed window swapchain fence is outside the submitted timeline");
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
            snapshot_.in_flight_frame_count -= 1U;
            snapshot_.current_in_flight_bytes -= bytes_per_image;
        }
    }
    snapshot_.completed_fence_value = completed_fence_value;
    return true;
}

NativeWindowSwapchainSnapshot
ReferenceNativeWindowSwapchainApi::snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void ReferenceNativeWindowSwapchainApi::shutdown() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    reset_images_locked();
    const NativeWindowSwapchainCapabilities capabilities_value =
        snapshot_.capabilities;
    const std::uint64_t peak_surface = snapshot_.peak_surface_bytes;
    const std::uint64_t peak_in_flight = snapshot_.peak_in_flight_bytes;
    snapshot_ = {};
    snapshot_.capabilities = capabilities_value;
    snapshot_.peak_surface_bytes = peak_surface;
    snapshot_.peak_in_flight_bytes = peak_in_flight;
    next_image_index_ = 0U;
}

void ReferenceNativeWindowSwapchainApi::reset_images_locked() noexcept {
    for (ImageSlot& slot : images_) {
        slot = {};
    }
    snapshot_.configured_image_count = 0U;
    snapshot_.acquired_image_count = 0U;
    snapshot_.in_flight_frame_count = 0U;
    snapshot_.current_surface_bytes = 0U;
    snapshot_.current_in_flight_bytes = 0U;
}

void ReferenceNativeWindowSwapchainApi::set_next_acquire_status(
    NativeWindowAcquireStatus status) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    next_acquire_status_ = status;
}

void ReferenceNativeWindowSwapchainApi::set_next_present_status(
    NativeWindowPresentStatus status) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    next_present_status_ = status;
}

void ReferenceNativeWindowSwapchainApi::set_occluded(bool occluded) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (occluded && snapshot_.occluded == 0U) {
        snapshot_.occlusion_events += 1U;
    }
    snapshot_.occluded = occluded ? 1U : 0U;
}

void ReferenceNativeWindowSwapchainApi::set_device_lost(bool lost) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lost && snapshot_.device_lost == 0U) {
        snapshot_.device_lost_events += 1U;
    }
    snapshot_.device_lost = lost ? 1U : 0U;
}

NativeWindowSwapchainCapabilities default_native_window_swapchain_capabilities(
    NativeGpuApiKind kind,
    NativeWindowSystem system) noexcept {
    NativeWindowSwapchainCapabilities output;
    output.flags =
        kNativeWindowSwapchainWindowSurface |
        kNativeWindowSwapchainResize |
        kNativeWindowSwapchainPartialPresent |
        kNativeWindowSwapchainOcclusion;
    output.minimum_image_count = 2U;
    output.maximum_image_count = 8U;
    output.maximum_frames_in_flight = 4U;
    output.maximum_damage_rects = 64U;
    output.maximum_width = 16'384U;
    output.maximum_height = 16'384U;
    output.maximum_surface_bytes = 512U * 1024U * 1024U;
    if (!window_system_supported(kind, system)) {
        output.flags = 0U;
        output.maximum_image_count = 0U;
        output.maximum_frames_in_flight = 0U;
        output.maximum_damage_rects = 0U;
        output.maximum_width = 0U;
        output.maximum_height = 0U;
        output.maximum_surface_bytes = 0U;
        return output;
    }
    if (kind == NativeGpuApiKind::Vulkan) {
        output.flags |=
            kNativeWindowSwapchainMailbox |
            kNativeWindowSwapchainImmediate;
    } else if (kind == NativeGpuApiKind::Metal) {
        output.flags |= kNativeWindowSwapchainMailbox;
    } else if (kind == NativeGpuApiKind::Direct3D12) {
        output.flags |=
            kNativeWindowSwapchainImmediate |
            kNativeWindowSwapchainTearing;
    }
    return output;
}

NativeWindowSwapchainLimits default_native_window_swapchain_limits(
    NativeGpuApiKind kind,
    NativeWindowSystem system) noexcept {
    const NativeWindowSwapchainCapabilities capabilities_value =
        default_native_window_swapchain_capabilities(kind, system);
    NativeWindowSwapchainLimits output;
    if (capabilities_value.flags == 0U) {
        return output;
    }
    output.maximum_image_count = 3U;
    output.maximum_frames_in_flight = 2U;
    output.maximum_damage_rects = 32U;
    output.maximum_width = 8'192U;
    output.maximum_height = 8'192U;
    output.maximum_surface_bytes = 256U * 1024U * 1024U;
    output.maximum_in_flight_bytes = 128U * 1024U * 1024U;
    return output;
}

} // namespace zevryon::text
