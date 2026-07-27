#include "native_gpu_sdk_execution.hpp"

#if defined(ZEVRYON_HAS_METAL_SDK)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

namespace zevryon::text {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

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

MTLPixelFormat map_format(GpuSurfaceFormat format) noexcept {
    return format == GpuSurfaceFormat::Rgba8Unorm
        ? MTLPixelFormatRGBA8Unorm
        : MTLPixelFormatBGRA8Unorm;
}

class MetalNativeGpuSdkApi final : public NativeGpuSdkApi {
public:
    MetalNativeGpuSdkApi() noexcept {
        snapshot_.probe.api_kind = NativeGpuApiKind::Metal;
        snapshot_.probe.availability = NativeGpuSdkAvailability::CompileOnly;
        snapshot_.probe.api_major = 3U;
        snapshot_.probe.flags = kNativeGpuSdkOffscreenSurface |
                                kNativeGpuSdkUnifiedMemory;
    }

    ~MetalNativeGpuSdkApi() override { shutdown(); }

    NativeGpuApiKind kind() const noexcept override { return NativeGpuApiKind::Metal; }

    NativeGpuSdkProbe probe() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_.probe;
    }

    bool initialize(
        const NativeGpuSdkConfig& config,
        NativeGpuSdkError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (config.api_kind != NativeGpuApiKind::Metal ||
            config.device_generation == 0U || config.runtime_generation == 0U ||
            config.window.system != NativeWindowSystem::Headless) {
            return fail(error, NativeGpuSdkErrorKind::InvalidInput,
                        "invalid Metal SDK configuration");
        }
        shutdown_locked();
        @autoreleasepool {
            device_ = MTLCreateSystemDefaultDevice();
            if (device_ == nil) {
                return fail(error, NativeGpuSdkErrorKind::RuntimeUnavailable,
                            "MTLCreateSystemDefaultDevice returned nil");
            }
            queue_ = [device_ newCommandQueue];
            if (queue_ == nil) {
                shutdown_locked();
                return fail(error, NativeGpuSdkErrorKind::QueueCreationFailed,
                            "Metal command queue creation failed");
            }
            config_ = config;
            snapshot_.config = config;
            snapshot_.probe.availability = NativeGpuSdkAvailability::RuntimeReady;
            snapshot_.probe.flags = kNativeGpuSdkRealDevice |
                                    kNativeGpuSdkOffscreenSurface |
                                    kNativeGpuSdkUnifiedMemory;
            snapshot_.probe.runtime_generation = config.runtime_generation;
            snapshot_.probe.vendor_id = 0x106BU;
            snapshot_.probe.device_id = static_cast<std::uint32_t>([device_ registryID] & 0xFFFFFFFFULL);
            std::uint64_t checksum = kFnvOffset;
            const std::uint64_t registry_id = [device_ registryID];
            hash_value(&checksum, registry_id);
            hash_value(&checksum, config.runtime_generation);
            snapshot_.probe.checksum = checksum;
            snapshot_.initialized_devices += 1U;
            initialized_ = true;
            return true;
        }
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
            image_count > textures_.size()) {
            return fail(error, NativeGpuSdkErrorKind::InvalidInput,
                        "invalid Metal offscreen surface configuration");
        }
        destroy_textures_locked();
        const std::uint64_t bytes_per_image =
            static_cast<std::uint64_t>(surface.width) * surface.height * 4U;
        if (surface.width != 0U && bytes_per_image / surface.width / 4U != surface.height) {
            return fail(error, NativeGpuSdkErrorKind::AggregateOverflow,
                        "Metal offscreen image byte count overflowed");
        }
        if (bytes_per_image > config_.limits.maximum_device_local_bytes / image_count) {
            return fail(error, NativeGpuSdkErrorKind::ResourceBudgetExceeded,
                        "Metal offscreen texture ring exceeds device-local budget");
        }
        @autoreleasepool {
            for (std::uint32_t index = 0U; index < image_count; ++index) {
                MTLTextureDescriptor* descriptor =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:map_format(surface.format)
                                                                  width:surface.width
                                                                 height:surface.height
                                                              mipmapped:NO];
                descriptor.usage = MTLTextureUsageRenderTarget |
                                   MTLTextureUsageShaderRead |
                                   MTLTextureUsageShaderWrite;
                descriptor.storageMode = MTLStorageModePrivate;
                textures_[index].texture = [device_ newTextureWithDescriptor:descriptor];
                if (textures_[index].texture == nil) {
                    destroy_textures_locked();
                    return fail(error, NativeGpuSdkErrorKind::ResourceAllocationFailed,
                                "Metal offscreen texture creation failed");
                }
                textures_[index].native_resource_id = next_resource_id_++;
                textures_[index].generation = next_image_generation_++;
                textures_[index].allocated_bytes = bytes_per_image;
            }
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
                        "invalid or stale Metal offscreen acquire request");
        }
        const std::uint32_t index = next_image_index_++ % image_count_;
        image->image.device_generation = config_.device_generation;
        image->image.surface_id = surface.surface_id;
        image->image.surface_generation = surface.generation_id;
        image->image.image_generation = textures_[index].generation;
        image->image.image_index = index;
        image->image.flags = 0U;
        image->driver_generation = config_.runtime_generation;
        image->native_resource_id = textures_[index].native_resource_id;
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
            submission.api_kind != NativeGpuApiKind::Metal ||
            !(submission.surface == surface_) ||
            submission.image.image.device_generation != config_.device_generation ||
            submission.image.driver_generation != config_.runtime_generation ||
            submission.image.image.image_index >= image_count_) {
            snapshot_.stale_rejections += 1U;
            return fail(error, NativeGpuSdkErrorKind::StaleGeneration,
                        "Metal submission references stale execution state");
        }
        if (submission.commands.size() > config_.limits.maximum_submission_commands ||
            submission.descriptors.size() > config_.limits.maximum_descriptors) {
            return fail(error, NativeGpuSdkErrorKind::ResourceBudgetExceeded,
                        "Metal submission exceeds bounded command or descriptor limits");
        }
        const std::uint32_t image_index = submission.image.image.image_index;
        TextureSlot& slot = textures_[image_index];
        if (submission.image.native_resource_id != slot.native_resource_id ||
            submission.image.image.image_generation != slot.generation) {
            snapshot_.stale_rejections += 1U;
            return fail(error, NativeGpuSdkErrorKind::StaleGeneration,
                        "Metal acquired image generation is stale");
        }
        @autoreleasepool {
            id<MTLCommandBuffer> command_buffer = [queue_ commandBuffer];
            if (command_buffer == nil) {
                return fail(error, NativeGpuSdkErrorKind::CommandEncodingFailed,
                            "Metal command buffer creation failed");
            }
            MTLRenderPassDescriptor* render_pass = [MTLRenderPassDescriptor renderPassDescriptor];
            render_pass.colorAttachments[0].texture = slot.texture;
            render_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            render_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            render_pass.colorAttachments[0].clearColor = MTLClearColorMake(
                static_cast<double>((submission.encoded_checksum >> 0U) & 0xFFU) / 255.0,
                static_cast<double>((submission.encoded_checksum >> 8U) & 0xFFU) / 255.0,
                static_cast<double>((submission.encoded_checksum >> 16U) & 0xFFU) / 255.0,
                1.0);
            id<MTLRenderCommandEncoder> encoder =
                [command_buffer renderCommandEncoderWithDescriptor:render_pass];
            if (encoder == nil) {
                return fail(error, NativeGpuSdkErrorKind::CommandEncodingFailed,
                            "Metal render command encoder creation failed");
            }
            [encoder endEncoding];
            [command_buffer commit];
            [command_buffer waitUntilCompleted];
            if (command_buffer.status == MTLCommandBufferStatusError) {
                const NSError* native_error = command_buffer.error;
                const std::int64_t code = native_error == nil ? 0 : native_error.code;
                snapshot_.device_lost_events += 1U;
                return fail(error, NativeGpuSdkErrorKind::SubmissionFailed,
                            "Metal command buffer completed with an error", code);
            }
        }

        const std::uint64_t signal = next_fence_value_++;
        if (signal <= snapshot_.last_submitted_fence_value) {
            return fail(error, NativeGpuSdkErrorKind::FenceRegression,
                        "Metal software fence timeline regressed");
        }
        std::uint64_t checksum = kFnvOffset;
        hash_value(&checksum, submission.encoded_checksum);
        hash_value(&checksum, submission.frame_id);
        hash_value(&checksum, submission.ticket_id);
        hash_value(&checksum, submission.commands.size());
        hash_value(&checksum, submission.barriers.size());
        hash_value(&checksum, submission.descriptors.size());
        hash_value(&checksum, slot.native_resource_id);
        hash_value(&checksum, snapshot_.probe.device_id);

        receipt->api_kind = NativeGpuApiKind::Metal;
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
                        "Metal completion fence is outside the submitted timeline");
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
    struct TextureSlot final {
        id<MTLTexture> texture{nil};
        std::uint64_t native_resource_id{0};
        std::uint64_t generation{0};
        std::uint64_t allocated_bytes{0};
    };

    void destroy_textures_locked() noexcept {
        for (TextureSlot& slot : textures_) {
            slot.texture = nil;
            slot.native_resource_id = 0U;
            slot.generation = 0U;
            slot.allocated_bytes = 0U;
        }
        image_count_ = 0U;
        next_image_index_ = 0U;
        snapshot_.configured_image_count = 0U;
        snapshot_.current_device_local_bytes = 0U;
        snapshot_.surface = {};
        surface_ = {};
    }

    void shutdown_locked() noexcept {
        destroy_textures_locked();
        queue_ = nil;
        device_ = nil;
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
    id<MTLDevice> device_{nil};
    id<MTLCommandQueue> queue_{nil};
    std::array<TextureSlot, 16U> textures_{};
    std::uint32_t image_count_{0};
    std::uint32_t next_image_index_{0};
    std::uint64_t next_image_generation_{1};
    std::uint64_t next_resource_id_{1};
    std::uint64_t next_fence_value_{1};
    bool initialized_{false};
};

} // namespace

std::unique_ptr<NativeGpuSdkApi> make_metal_native_gpu_sdk_api() noexcept {
    try {
        return std::make_unique<MetalNativeGpuSdkApi>();
    } catch (...) {
        return nullptr;
    }
}

} // namespace zevryon::text

#endif // ZEVRYON_HAS_METAL_SDK
