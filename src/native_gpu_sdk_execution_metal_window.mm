#include "native_metal_window.hpp"
#include "native_metal_window_context.hpp"

#if defined(ZEVRYON_HAS_METAL_WINDOW_SWAPCHAIN)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <limits>
#include <memory>
#include <mutex>
#include <new>

namespace zevryon::text {
namespace {

using detail::MetalWindowSharedContext;

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

template <typename T>
void hash_value(std::uint64_t* hash, const T& value) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        *hash ^= bytes[index];
        *hash *= kFnvPrime;
    }
}

template <typename T>
std::uint64_t opaque_pointer_id(T* pointer) noexcept {
    return static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(pointer));
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

class MetalWindowNativeGpuSdkApi final : public NativeGpuSdkApi {
public:
    MetalWindowNativeGpuSdkApi() noexcept {
        snapshot_.probe.api_kind = NativeGpuApiKind::Metal;
        snapshot_.probe.availability = NativeGpuSdkAvailability::CompileOnly;
        snapshot_.probe.api_major = 3U;
        snapshot_.probe.flags = kNativeGpuSdkWindowSurface |
                                kNativeGpuSdkUnifiedMemory;
    }

    ~MetalWindowNativeGpuSdkApi() override { shutdown(); }

    NativeGpuApiKind kind() const noexcept override {
        return NativeGpuApiKind::Metal;
    }

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
            config.device_generation == 0U ||
            config.runtime_generation == 0U ||
            config.window.system != NativeWindowSystem::CocoaLayer ||
            config.window.generation == 0U ||
            config.window.window_or_layer == 0U) {
            return fail(error, NativeGpuSdkErrorKind::InvalidInput,
                        "invalid Metal window SDK configuration");
        }
        shutdown_locked();

        @autoreleasepool {
            CAMetalLayer* layer = (__bridge CAMetalLayer*)(
                reinterpret_cast<void*>(
                    static_cast<std::uintptr_t>(config.window.window_or_layer)));
            if (layer == nil || ![layer isKindOfClass:[CAMetalLayer class]]) {
                return fail(error, NativeGpuSdkErrorKind::InvalidInput,
                            "Cocoa window handle does not reference CAMetalLayer");
            }

            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (device == nil) {
                return fail(error, NativeGpuSdkErrorKind::RuntimeUnavailable,
                            "MTLCreateSystemDefaultDevice returned nil");
            }
            id<MTLCommandQueue> queue = [device newCommandQueue];
            if (queue == nil) {
                return fail(error, NativeGpuSdkErrorKind::QueueCreationFailed,
                            "Metal presentation command queue creation failed");
            }

            auto* staged = new (std::nothrow) MetalWindowSharedContext();
            if (staged == nullptr) {
                return fail(error, NativeGpuSdkErrorKind::ResourceAllocationFailed,
                            "Metal retained context allocation failed");
            }
            staged->window = config.window;
            staged->device = device;
            staged->queue = queue;
            staged->layer = layer;
            staged->device_generation = config.device_generation;
            staged->runtime_generation = config.runtime_generation;
            context_ = staged;

            snapshot_.config = config;
            snapshot_.probe.availability = NativeGpuSdkAvailability::RuntimeReady;
            snapshot_.probe.flags = kNativeGpuSdkRealDevice |
                                    kNativeGpuSdkWindowSurface |
                                    kNativeGpuSdkUnifiedMemory;
            snapshot_.probe.runtime_generation = config.runtime_generation;
            snapshot_.probe.vendor_id = 0x106BU;
            snapshot_.probe.device_id = static_cast<std::uint32_t>(
                [device registryID] & 0xFFFFFFFFULL);
            snapshot_.probe.shared_system_memory_bytes =
                static_cast<std::uint64_t>([NSProcessInfo processInfo].physicalMemory);
            std::uint64_t checksum = kFnvOffset;
            const std::uint64_t registry_id = [device registryID];
            hash_value(&checksum, registry_id);
            hash_value(&checksum, config.device_generation);
            hash_value(&checksum, config.runtime_generation);
            hash_value(&checksum, config.window.generation);
            snapshot_.probe.checksum = checksum;
            snapshot_.initialized_devices += 1U;
            initialized_ = true;
            return true;
        }
    }

    bool export_context(
        NativeGpuSdkContextHandle* context,
        NativeGpuSdkError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (context == nullptr || !initialized_ || context_ == nullptr ||
            context_->owner_released.load(std::memory_order_acquire) != 0U) {
            return fail(error, NativeGpuSdkErrorKind::StaleGeneration,
                        "Metal window context is unavailable or stale");
        }
        *context = {};
        context->api_kind = NativeGpuApiKind::Metal;
        context->flags = kNativeGpuSdkContextDeviceValid |
                         kNativeGpuSdkContextGraphicsQueueValid |
                         kNativeGpuSdkContextPresentQueueValid |
                         kNativeGpuSdkContextSharedGraphicsPresentQueue |
                         detail::kNativeGpuSdkContextRetainedLease |
                         detail::kNativeGpuSdkContextMetalWindow;
        context->device_generation = context_->device_generation;
        context->runtime_generation = context_->runtime_generation;
        context->instance_or_factory = opaque_pointer_id(context_);
        context->physical_device_or_adapter =
            static_cast<std::uint64_t>([context_->device registryID]);
        context->device = opaque_pointer_id((__bridge void*)context_->device);
        context->graphics_queue =
            opaque_pointer_id((__bridge void*)context_->queue);
        context->present_queue = context->graphics_queue;
        return true;
    }

    bool configure_offscreen_surface(
        const GpuSurfaceDescriptor&,
        std::uint32_t,
        NativeGpuSdkError* error) noexcept override {
        clear_error(error);
        return fail(error, NativeGpuSdkErrorKind::UnsupportedBackend,
                    "Metal window owner does not allocate an offscreen texture ring");
    }

    bool acquire_image(
        const GpuSurfaceDescriptor&,
        std::uint64_t,
        NativePlatformSwapchainImage*,
        NativeAcquireStatus*,
        NativeGpuSdkError* error) noexcept override {
        clear_error(error);
        return fail(error, NativeGpuSdkErrorKind::UnsupportedBackend,
                    "Metal window drawables are acquired through NativeWindowSwapchainApi");
    }

    bool execute_submission(
        const NativePlatformSubmission&,
        NativeGpuSdkSubmissionReceipt*,
        NativeGpuSdkError* error) noexcept override {
        clear_error(error);
        return fail(error, NativeGpuSdkErrorKind::UnsupportedBackend,
                    "Metal window submissions are executed through NativeWindowSwapchainApi");
    }

    bool retire_completed(
        std::uint64_t completed_fence_value,
        NativeGpuSdkError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (completed_fence_value < snapshot_.completed_fence_value ||
            completed_fence_value > snapshot_.last_submitted_fence_value) {
            return fail(error, NativeGpuSdkErrorKind::FenceRegression,
                        "Metal window owner completion fence is invalid");
        }
        snapshot_.completed_fence_value = completed_fence_value;
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
    void shutdown_locked() noexcept {
        if (context_ != nullptr) {
            detail::release_metal_window_owner(context_);
            context_ = nullptr;
        }
        snapshot_.configured_image_count = 0U;
        snapshot_.in_flight_frame_count = 0U;
        snapshot_.current_staging_bytes = 0U;
        snapshot_.current_device_local_bytes = 0U;
        snapshot_.last_submitted_fence_value = 0U;
        snapshot_.completed_fence_value = 0U;
        initialized_ = false;
    }

    mutable std::mutex mutex_;
    NativeGpuSdkSnapshot snapshot_;
    MetalWindowSharedContext* context_{nullptr};
    bool initialized_{false};
};

} // namespace

namespace detail {

MetalWindowSharedContext* retain_metal_window_context(
    const NativeGpuSdkContextHandle& context) noexcept {
    if (context.api_kind != NativeGpuApiKind::Metal ||
        (context.flags & kNativeGpuSdkContextRetainedLease) == 0U ||
        (context.flags & kNativeGpuSdkContextMetalWindow) == 0U ||
        context.instance_or_factory == 0U) {
        return nullptr;
    }
    auto* shared = reinterpret_cast<MetalWindowSharedContext*>(
        static_cast<std::uintptr_t>(context.instance_or_factory));
    if (shared->owner_released.load(std::memory_order_acquire) != 0U) {
        return nullptr;
    }
    std::uint32_t current = shared->references.load(std::memory_order_acquire);
    while (current != 0U &&
           current != (std::numeric_limits<std::uint32_t>::max)()) {
        if (shared->references.compare_exchange_weak(
                current, current + 1U,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            if (shared->owner_released.load(std::memory_order_acquire) == 0U) {
                return shared;
            }
            release_metal_window_context(shared);
            return nullptr;
        }
    }
    return nullptr;
}

void release_metal_window_context(MetalWindowSharedContext* context) noexcept {
    if (context == nullptr) {
        return;
    }
    if (context->references.fetch_sub(1U, std::memory_order_acq_rel) != 1U) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(context->device_mutex);
        if (context->layer != nil && context->layer.device == context->device) {
            context->layer.device = nil;
        }
        context->layer = nil;
        context->queue = nil;
        context->device = nil;
    }
    delete context;
}

void release_metal_window_owner(MetalWindowSharedContext* context) noexcept {
    if (context != nullptr) {
        context->owner_released.store(1U, std::memory_order_release);
    }
    release_metal_window_context(context);
}

} // namespace detail

std::unique_ptr<NativeGpuSdkApi>
make_metal_window_native_gpu_sdk_api() noexcept {
    try {
        return std::make_unique<MetalWindowNativeGpuSdkApi>();
    } catch (...) {
        return nullptr;
    }
}

} // namespace zevryon::text

#endif // ZEVRYON_HAS_METAL_WINDOW_SWAPCHAIN
