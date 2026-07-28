#include "native_metal_window.hpp"
#include "native_metal_window_context.hpp"

#if defined(ZEVRYON_HAS_METAL_WINDOW_SWAPCHAIN)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>

namespace zevryon::text {
namespace {

using detail::MetalWindowSharedContext;
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

std::uint64_t object_id(id object) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
        (__bridge void*)object));
}

MTLPixelFormat map_format(GpuSurfaceFormat format) noexcept {
    return format == GpuSurfaceFormat::Rgba8Unorm
        ? MTLPixelFormatRGBA8Unorm
        : MTLPixelFormatBGRA8Unorm;
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
    if (width > (std::numeric_limits<std::uint64_t>::max)() / height) {
        return false;
    }
    const std::uint64_t pixels = width * height;
    if (pixels > (std::numeric_limits<std::uint64_t>::max)() / kBytesPerPixel) {
        return false;
    }
    *bytes_per_image = pixels * kBytesPerPixel;
    if (*bytes_per_image >
        (std::numeric_limits<std::uint64_t>::max)() / image_count) {
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

class MetalNativeWindowSwapchainApi final : public NativeWindowSwapchainApi {
public:
    MetalNativeWindowSwapchainApi() noexcept {
        snapshot_.capabilities = default_native_window_swapchain_capabilities(
            NativeGpuApiKind::Metal,
            NativeWindowSystem::CocoaLayer);
        snapshot_.capabilities.flags =
            kNativeWindowSwapchainWindowSurface |
            kNativeWindowSwapchainResize |
            kNativeWindowSwapchainImmediate |
            kNativeWindowSwapchainOcclusion;
        snapshot_.capabilities.minimum_image_count = 2U;
        snapshot_.capabilities.maximum_image_count = 3U;
        snapshot_.capabilities.maximum_frames_in_flight = 2U;
        snapshot_.capabilities.maximum_damage_rects = 64U;
    }

    ~MetalNativeWindowSwapchainApi() override { shutdown(); }

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
                        "Metal window swapchain is already configured");
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
                        "Metal resize does not advance a valid surface generation");
        }
        std::uint64_t bytes_per_image = 0U;
        std::uint64_t total_bytes = 0U;
        if (!checked_surface_bytes(
                surface,
                snapshot_.config.image_count,
                &bytes_per_image,
                &total_bytes)) {
            return fail(error, NativeWindowSwapchainErrorKind::ArithmeticOverflow,
                        "Metal resized surface byte count overflowed");
        }
        if (total_bytes > snapshot_.config.limits.maximum_surface_bytes ||
            total_bytes > snapshot_.capabilities.maximum_surface_bytes ||
            bytes_per_image >
                snapshot_.config.limits.maximum_in_flight_bytes /
                    snapshot_.config.limits.maximum_frames_in_flight) {
            return fail(error, NativeWindowSwapchainErrorKind::ResourceBudgetExceeded,
                        "Metal resized surface exceeds configured budgets");
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
                        "Metal layer recreation was not requested");
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
        if (ticket_id == 0U || image == nullptr || status == nullptr ||
            snapshot_.configured == 0U || context_ == nullptr || layer_ == nil) {
            return fail(error, NativeWindowSwapchainErrorKind::AcquireFailed,
                        "invalid Metal drawable acquire request");
        }
        if (snapshot_.device_lost != 0U) {
            *status = NativeWindowAcquireStatus::DeviceLost;
            return true;
        }
        if (snapshot_.out_of_date != 0U) {
            *status = NativeWindowAcquireStatus::OutOfDate;
            return true;
        }
        if (snapshot_.acquired_image_count + snapshot_.in_flight_frame_count >=
            snapshot_.config.limits.maximum_frames_in_flight) {
            *status = NativeWindowAcquireStatus::NotReady;
            return true;
        }

        std::uint32_t index = snapshot_.configured_image_count;
        for (std::uint32_t candidate = 0U;
             candidate < snapshot_.configured_image_count;
             ++candidate) {
            if (slots_[candidate].acquired == 0U &&
                slots_[candidate].in_flight == 0U) {
                index = candidate;
                break;
            }
        }
        if (index >= snapshot_.configured_image_count) {
            *status = NativeWindowAcquireStatus::NotReady;
            return true;
        }

        @autoreleasepool {
            id<CAMetalDrawable> drawable = [layer_ nextDrawable];
            if (drawable == nil || drawable.texture == nil) {
                if (snapshot_.occluded == 0U) {
                    snapshot_.occlusion_events += 1U;
                }
                snapshot_.occluded = 1U;
                *status = NativeWindowAcquireStatus::Occluded;
                return true;
            }
            const id<MTLTexture> texture = drawable.texture;
            if (texture.width != snapshot_.config.surface.width ||
                texture.height != snapshot_.config.surface.height) {
                if (snapshot_.out_of_date == 0U) {
                    snapshot_.out_of_date_events += 1U;
                }
                snapshot_.out_of_date = 1U;
                *status = NativeWindowAcquireStatus::OutOfDate;
                return true;
            }

            ImageSlot& slot = slots_[index];
            slot.drawable = drawable;
            slot.texture = texture;
            slot.upload_buffer = nil;
            slot.command_buffer = nil;
            slot.acquired = 1U;
            slot.in_flight = 0U;
            slot.fence_value = 0U;
            slot.image = {};
            slot.image.image.image.device_generation =
                snapshot_.config.context.device_generation;
            slot.image.image.image.surface_id =
                snapshot_.config.surface.surface_id;
            slot.image.image.image.surface_generation =
                snapshot_.config.surface.generation_id;
            slot.image.image.image.image_generation = next_image_generation_++;
            slot.image.image.image.image_index = index;
            slot.image.image.image.flags = 0U;
            slot.image.image.driver_generation =
                snapshot_.config.context.runtime_generation;
            slot.image.image.native_resource_id = object_id(texture);
            slot.image.image.state = NativePlatformResourceState::Present;
            slot.image.swapchain_generation =
                snapshot_.config.swapchain_generation;
            slot.image.acquire_serial = next_acquire_serial_++;
            slot.image.present_serial = 0U;
            slot.image.flags = kNativeWindowSwapchainImageAcquired;
            *image = slot.image;
            *status = NativeWindowAcquireStatus::Acquired;
            snapshot_.occluded = 0U;
            snapshot_.acquired_images += 1U;
            snapshot_.acquired_image_count += 1U;
            return true;
        }
    }

    bool present(
        const NativeWindowPresentRequest& request,
        NativeWindowPresentReceipt* receipt,
        NativeWindowSwapchainError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (receipt == nullptr || snapshot_.configured == 0U ||
            context_ == nullptr || layer_ == nil || request.frame_id == 0U ||
            request.ticket_id == 0U ||
            request.image.image.image.image_index >=
                snapshot_.configured_image_count) {
            return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                        "invalid Metal drawable present request");
        }
        const std::uint32_t index = request.image.image.image.image_index;
        ImageSlot& slot = slots_[index];
        if (slot.acquired == 0U || slot.drawable == nil || slot.texture == nil ||
            request.image.swapchain_generation !=
                snapshot_.config.swapchain_generation ||
            request.image.image.image.device_generation !=
                snapshot_.config.context.device_generation ||
            request.image.image.driver_generation !=
                snapshot_.config.context.runtime_generation ||
            request.image.image.image.surface_generation !=
                snapshot_.config.surface.generation_id ||
            request.image.image.image.image_generation !=
                slot.image.image.image.image_generation ||
            request.image.acquire_serial != slot.image.acquire_serial ||
            request.image.image.native_resource_id !=
                slot.image.image.native_resource_id) {
            snapshot_.stale_rejections += 1U;
            return fail(error, NativeWindowSwapchainErrorKind::StaleGeneration,
                        "Metal present references a stale or unowned drawable");
        }
        if (request.damage_rects.size() >
            snapshot_.config.limits.maximum_damage_rects) {
            return fail(error, NativeWindowSwapchainErrorKind::ResourceBudgetExceeded,
                        "Metal present damage count exceeds configured limits");
        }
        for (const NativeDamageRect& rect : request.damage_rects) {
            if (!damage_rect_valid(rect, snapshot_.config.surface)) {
                return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                            "Metal present damage rectangle is outside the surface");
            }
        }
        if (!native_window_pixel_buffer_valid(
                request.pixel_buffer, snapshot_.config.surface)) {
            return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                        "Metal pixel buffer does not match the drawable surface");
        }
        if ((request.flags & kNativeWindowPresentAllowTearing) != 0U) {
            return fail(error, NativeWindowSwapchainErrorKind::UnsupportedPresentMode,
                        "Metal CAMetalLayer does not expose explicit tearing control");
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
            release_acquired_slot_locked(slot);
            snapshot_.skipped_frames += 1U;
            receipt->status = NativeWindowPresentStatus::SkippedNoDamage;
            receipt->signal_fence_value = snapshot_.last_submitted_fence_value;
            return true;
        }
        if (snapshot_.in_flight_frame_count >=
            snapshot_.config.limits.maximum_frames_in_flight) {
            return fail(error, NativeWindowSwapchainErrorKind::Backpressure,
                        "maximum Metal frames in flight was reached");
        }

        @autoreleasepool {
            id<MTLCommandBuffer> command_buffer = [context_->queue commandBuffer];
            if (command_buffer == nil) {
                return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                            "Metal presentation command buffer creation failed");
            }
            if (!request.pixel_buffer.empty()) {
                id<MTLBuffer> upload = [context_->device
                    newBufferWithBytes:request.pixel_buffer.bytes.data()
                    length:request.pixel_buffer.bytes.size()
                    options:MTLResourceStorageModeShared];
                if (upload == nil) {
                    return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                                "Metal pixel upload buffer creation failed");
                }
                id<MTLBlitCommandEncoder> blit =
                    [command_buffer blitCommandEncoder];
                if (blit == nil) {
                    return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                                "Metal pixel blit encoder creation failed");
                }
                [blit copyFromBuffer:upload
                        sourceOffset:0U
                   sourceBytesPerRow:request.pixel_buffer.row_bytes
                 sourceBytesPerImage:static_cast<NSUInteger>(
                     request.pixel_buffer.row_bytes) *
                     request.pixel_buffer.height
                          sourceSize:MTLSizeMake(
                              request.pixel_buffer.width,
                              request.pixel_buffer.height, 1U)
                           toTexture:slot.texture
                    destinationSlice:0U
                    destinationLevel:0U
                   destinationOrigin:MTLOriginMake(0U, 0U, 0U)];
                [blit endEncoding];
                slot.upload_buffer = upload;
            } else {
                MTLRenderPassDescriptor* pass =
                    [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = slot.texture;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.colorAttachments[0].clearColor = MTLClearColorMake(
                    static_cast<double>((request.command_checksum >> 0U) & 0xFFU) / 255.0,
                    static_cast<double>((request.command_checksum >> 8U) & 0xFFU) / 255.0,
                    static_cast<double>((request.command_checksum >> 16U) & 0xFFU) / 255.0,
                    1.0);
                id<MTLRenderCommandEncoder> encoder =
                    [command_buffer renderCommandEncoderWithDescriptor:pass];
                if (encoder == nil) {
                    return fail(error, NativeWindowSwapchainErrorKind::PresentFailed,
                                "Metal presentation render encoder creation failed");
                }
                [encoder endEncoding];
            }
            [command_buffer presentDrawable:slot.drawable];

            const std::uint64_t signal = next_fence_value_++;
            slot.command_buffer = command_buffer;
            slot.acquired = 0U;
            slot.in_flight = 1U;
            slot.fence_value = signal;
            slot.image.present_serial = next_present_serial_++;
            slot.image.flags = 0U;
            [command_buffer commit];

            const std::uint64_t bytes_per_image =
                snapshot_.current_surface_bytes /
                snapshot_.configured_image_count;
            snapshot_.acquired_image_count -= 1U;
            snapshot_.in_flight_frame_count += 1U;
            snapshot_.current_in_flight_bytes += bytes_per_image;
            snapshot_.peak_in_flight_bytes = std::max(
                snapshot_.peak_in_flight_bytes,
                snapshot_.current_in_flight_bytes);
            snapshot_.presented_frames += 1U;
            snapshot_.last_submitted_fence_value = signal;

            receipt->image = slot.image;
            receipt->status = NativeWindowPresentStatus::Presented;
            receipt->signal_fence_value = signal;
            return true;
        }
    }

    bool retire_completed(
        std::uint64_t completed_fence_value,
        NativeWindowSwapchainError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (completed_fence_value < snapshot_.completed_fence_value ||
            completed_fence_value > snapshot_.last_submitted_fence_value) {
            return fail(error, NativeWindowSwapchainErrorKind::FenceRegression,
                        "completed Metal fence is outside the submitted timeline");
        }
        const std::uint64_t bytes_per_image =
            snapshot_.configured_image_count == 0U
            ? 0U
            : snapshot_.current_surface_bytes /
                snapshot_.configured_image_count;
        for (ImageSlot& slot : slots_) {
            if (slot.in_flight == 0U ||
                slot.fence_value > completed_fence_value) {
                continue;
            }
            id<MTLCommandBuffer> command_buffer = slot.command_buffer;
            if (command_buffer != nil) {
                [command_buffer waitUntilCompleted];
                if (command_buffer.status == MTLCommandBufferStatusError) {
                    const NSError* native_error = command_buffer.error;
                    const std::int64_t code =
                        native_error == nil ? 0 : native_error.code;
                    snapshot_.device_lost = 1U;
                    snapshot_.device_lost_events += 1U;
                    return fail(error, NativeWindowSwapchainErrorKind::DeviceLost,
                                "Metal presentation command buffer failed", code);
                }
            }
            clear_in_flight_slot_locked(slot);
            snapshot_.in_flight_frame_count -= 1U;
            snapshot_.current_in_flight_bytes -= bytes_per_image;
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
        shutdown_locked();
    }

private:
    struct ImageSlot final {
        NativeWindowSwapchainImage image;
        id<CAMetalDrawable> drawable{nil};
        id<MTLTexture> texture{nil};
        id<MTLBuffer> upload_buffer{nil};
        id<MTLCommandBuffer> command_buffer{nil};
        std::uint64_t fence_value{0U};
        std::uint8_t acquired{0U};
        std::uint8_t in_flight{0U};
        std::uint8_t reserved[6]{0, 0, 0, 0, 0, 0};
    };

    bool configure_locked(
        const NativeWindowSwapchainConfig& config,
        bool recreation,
        NativeWindowSwapchainError* error) noexcept {
        const bool require_native =
            (config.flags & kNativeWindowSwapchainRequireNativeContext) != 0U;
        const std::uint32_t required_flags =
            kNativeGpuSdkContextDeviceValid |
            kNativeGpuSdkContextGraphicsQueueValid |
            kNativeGpuSdkContextPresentQueueValid |
            detail::kNativeGpuSdkContextRetainedLease |
            detail::kNativeGpuSdkContextMetalWindow;
        if (config.context.api_kind != NativeGpuApiKind::Metal ||
            config.window.system != NativeWindowSystem::CocoaLayer ||
            config.window.generation == 0U ||
            config.window.window_or_layer == 0U ||
            config.surface.surface_id == 0U ||
            config.surface.generation_id == 0U ||
            config.surface.width == 0U || config.surface.height == 0U ||
            config.swapchain_generation == 0U ||
            config.image_count < 2U || config.image_count > 3U ||
            config.image_count > config.limits.maximum_image_count ||
            config.limits.maximum_frames_in_flight == 0U ||
            config.limits.maximum_frames_in_flight > 2U ||
            config.limits.maximum_frames_in_flight > config.image_count ||
            config.limits.maximum_damage_rects == 0U ||
            (require_native &&
             ((config.context.flags & required_flags) != required_flags ||
              config.context.device == 0U ||
              config.context.graphics_queue == 0U ||
              config.context.present_queue == 0U))) {
            return fail(error, NativeWindowSwapchainErrorKind::InvalidInput,
                        "invalid Metal CAMetalLayer configuration");
        }
        if (config.present_mode == NativePresentMode::Mailbox) {
            return fail(error, NativeWindowSwapchainErrorKind::UnsupportedPresentMode,
                        "CAMetalLayer does not expose mailbox present mode");
        }
        if (config.present_mode == NativePresentMode::Immediate &&
            (config.flags & kNativeWindowSwapchainAllowImmediate) == 0U) {
            return fail(error, NativeWindowSwapchainErrorKind::UnsupportedPresentMode,
                        "Metal immediate mode was not enabled");
        }
        if (config.surface.width > config.limits.maximum_width ||
            config.surface.height > config.limits.maximum_height ||
            config.surface.width > snapshot_.capabilities.maximum_width ||
            config.surface.height > snapshot_.capabilities.maximum_height) {
            return fail(error, NativeWindowSwapchainErrorKind::ResourceBudgetExceeded,
                        "Metal layer extent exceeds configured limits");
        }

        std::uint64_t bytes_per_image = 0U;
        std::uint64_t total_bytes = 0U;
        if (!checked_surface_bytes(
                config.surface,
                config.image_count,
                &bytes_per_image,
                &total_bytes)) {
            return fail(error, NativeWindowSwapchainErrorKind::ArithmeticOverflow,
                        "Metal layer surface byte count overflowed");
        }
        if (total_bytes > config.limits.maximum_surface_bytes ||
            total_bytes > snapshot_.capabilities.maximum_surface_bytes ||
            bytes_per_image >
                config.limits.maximum_in_flight_bytes /
                    config.limits.maximum_frames_in_flight) {
            return fail(error, NativeWindowSwapchainErrorKind::ResourceBudgetExceeded,
                        "Metal layer surface exceeds configured budgets");
        }

        MetalWindowSharedContext* retained = context_;
        if (!recreation) {
            retained = detail::retain_metal_window_context(config.context);
            if (retained == nullptr) {
                return fail(error,
                            NativeWindowSwapchainErrorKind::NativeContextUnavailable,
                            "Metal retained native context is unavailable");
            }
        } else if (context_ == nullptr ||
                   config.context.instance_or_factory !=
                       snapshot_.config.context.instance_or_factory ||
                   config.context.device_generation !=
                       snapshot_.config.context.device_generation ||
                   config.context.runtime_generation !=
                       snapshot_.config.context.runtime_generation ||
                   config.window.window_or_layer !=
                       snapshot_.config.window.window_or_layer ||
                   config.window.generation !=
                       snapshot_.config.window.generation ||
                   config.surface != snapshot_.pending_surface ||
                   config.swapchain_generation <=
                       snapshot_.config.swapchain_generation) {
            snapshot_.stale_rejections += 1U;
            return fail(error, NativeWindowSwapchainErrorKind::StaleGeneration,
                        "Metal recreation references stale context or layer state");
        }

        CAMetalLayer* layer = retained->layer;
        if (layer == nil ||
            object_id(layer) != config.window.window_or_layer ||
            retained->device == nil || retained->queue == nil ||
            retained->device_generation != config.context.device_generation ||
            retained->runtime_generation != config.context.runtime_generation ||
            object_id(retained->device) != config.context.device ||
            object_id(retained->queue) != config.context.graphics_queue ||
            config.context.graphics_queue != config.context.present_queue) {
            if (!recreation) {
                detail::release_metal_window_context(retained);
            }
            return fail(error,
                        NativeWindowSwapchainErrorKind::NativeContextUnavailable,
                        "Metal context does not match the exported device graph");
        }

        if (recreation) {
            if (!wait_and_release_all_locked(error)) {
                return false;
            }
        }

        @autoreleasepool {
            layer.device = retained->device;
            layer.pixelFormat = map_format(config.surface.format);
            layer.framebufferOnly = NO;
            layer.opaque = YES;
            layer.presentsWithTransaction = NO;
            layer.allowsNextDrawableTimeout = YES;
            if ([layer respondsToSelector:@selector(setMaximumDrawableCount:)]) {
                layer.maximumDrawableCount = config.image_count;
            }
            if ([layer respondsToSelector:@selector(setDisplaySyncEnabled:)]) {
                layer.displaySyncEnabled =
                    config.present_mode == NativePresentMode::Fifo;
            }
            layer.drawableSize = CGSizeMake(
                static_cast<CGFloat>(config.surface.width),
                static_cast<CGFloat>(config.surface.height));
        }

        if (!recreation) {
            context_ = retained;
            snapshot_.configurations += 1U;
        } else {
            snapshot_.recreations += 1U;
        }
        layer_ = layer;
        snapshot_.config = config;
        snapshot_.pending_surface = {};
        snapshot_.configured_image_count = config.image_count;
        snapshot_.acquired_image_count = 0U;
        snapshot_.in_flight_frame_count = 0U;
        snapshot_.current_surface_bytes = total_bytes;
        snapshot_.peak_surface_bytes = std::max(
            snapshot_.peak_surface_bytes,
            snapshot_.current_surface_bytes);
        snapshot_.current_in_flight_bytes = 0U;
        snapshot_.configured = 1U;
        snapshot_.out_of_date = 0U;
        snapshot_.occluded = 0U;
        snapshot_.device_lost = 0U;
        reset_slots_locked();
        return true;
    }

    bool wait_and_release_all_locked(
        NativeWindowSwapchainError* error) noexcept {
        for (ImageSlot& slot : slots_) {
            if (slot.in_flight != 0U && slot.command_buffer != nil) {
                [slot.command_buffer waitUntilCompleted];
                if (slot.command_buffer.status == MTLCommandBufferStatusError) {
                    const NSError* native_error = slot.command_buffer.error;
                    const std::int64_t code =
                        native_error == nil ? 0 : native_error.code;
                    snapshot_.device_lost = 1U;
                    snapshot_.device_lost_events += 1U;
                    return fail(error, NativeWindowSwapchainErrorKind::DeviceLost,
                                "Metal command buffer failed during recreation", code);
                }
            }
        }
        reset_slots_locked();
        snapshot_.acquired_image_count = 0U;
        snapshot_.in_flight_frame_count = 0U;
        snapshot_.current_in_flight_bytes = 0U;
        return true;
    }

    void release_acquired_slot_locked(ImageSlot& slot) noexcept {
        slot.drawable = nil;
        slot.texture = nil;
        slot.upload_buffer = nil;
        slot.command_buffer = nil;
        slot.acquired = 0U;
        slot.in_flight = 0U;
        slot.fence_value = 0U;
        slot.image = {};
        snapshot_.acquired_image_count -= 1U;
    }

    void clear_in_flight_slot_locked(ImageSlot& slot) noexcept {
        slot.drawable = nil;
        slot.texture = nil;
        slot.upload_buffer = nil;
        slot.command_buffer = nil;
        slot.acquired = 0U;
        slot.in_flight = 0U;
        slot.fence_value = 0U;
        slot.image = {};
    }

    void reset_slots_locked() noexcept {
        for (ImageSlot& slot : slots_) {
            slot.drawable = nil;
            slot.texture = nil;
            slot.upload_buffer = nil;
            slot.command_buffer = nil;
            slot.image = {};
            slot.fence_value = 0U;
            slot.acquired = 0U;
            slot.in_flight = 0U;
        }
    }

    void shutdown_locked() noexcept {
        for (ImageSlot& slot : slots_) {
            if (slot.in_flight != 0U && slot.command_buffer != nil) {
                [slot.command_buffer waitUntilCompleted];
            }
        }
        reset_slots_locked();
        if (context_ != nullptr) {
            detail::release_metal_window_context(context_);
            context_ = nullptr;
        }
        layer_ = nil;
        const NativeWindowSwapchainCapabilities capabilities_value =
            snapshot_.capabilities;
        const std::uint64_t peak_surface = snapshot_.peak_surface_bytes;
        const std::uint64_t peak_in_flight = snapshot_.peak_in_flight_bytes;
        snapshot_ = {};
        snapshot_.capabilities = capabilities_value;
        snapshot_.peak_surface_bytes = peak_surface;
        snapshot_.peak_in_flight_bytes = peak_in_flight;
    }

    mutable std::mutex mutex_;
    NativeWindowSwapchainSnapshot snapshot_;
    MetalWindowSharedContext* context_{nullptr};
    CAMetalLayer* layer_{nil};
    std::array<ImageSlot, 3U> slots_{};
    std::uint64_t next_image_generation_{1U};
    std::uint64_t next_acquire_serial_{1U};
    std::uint64_t next_present_serial_{1U};
    std::uint64_t next_fence_value_{1U};
};

} // namespace

std::unique_ptr<NativeWindowSwapchainApi>
make_metal_native_window_swapchain_api() noexcept {
    try {
        return std::make_unique<MetalNativeWindowSwapchainApi>();
    } catch (...) {
        return nullptr;
    }
}

bool native_metal_window_build_has_backend(
    NativeWindowSystem system) noexcept {
    return system == NativeWindowSystem::CocoaLayer;
}

bool native_window_swapchain_build_has_backend(
    NativeGpuApiKind kind,
    NativeWindowSystem system) noexcept {
    return kind == NativeGpuApiKind::Metal &&
        system == NativeWindowSystem::CocoaLayer;
}

} // namespace zevryon::text

#endif // ZEVRYON_HAS_METAL_WINDOW_SWAPCHAIN
