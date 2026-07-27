#include "native_vulkan_wsi.hpp"
#include "native_vulkan_wsi_context.hpp"

#if defined(ZEVRYON_HAS_VULKAN_WSI)

#if defined(ZEVRYON_VULKAN_WSI_HAS_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if defined(ZEVRYON_VULKAN_WSI_HAS_XCB)
#include <xcb/xcb.h>
#endif
#if defined(ZEVRYON_VULKAN_WSI_HAS_WAYLAND)
#include <wayland-client.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace zevryon::text {
namespace {

using detail::VulkanWsiSharedContext;

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
std::uint64_t opaque_handle_id(T handle) noexcept {
    if constexpr (std::is_pointer_v<T>) {
        return static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(handle));
    } else {
        return static_cast<std::uint64_t>(handle);
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
    VkResult result = VK_SUCCESS) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->native_code = static_cast<std::int64_t>(result);
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool instance_extension_available(const char* name) noexcept {
    try {
        std::uint32_t count = 0U;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) {
            return false;
        }
        std::vector<VkExtensionProperties> extensions(count);
        if (vkEnumerateInstanceExtensionProperties(
                nullptr, &count, extensions.data()) != VK_SUCCESS) {
            return false;
        }
        return std::any_of(
            extensions.begin(), extensions.end(),
            [name](const auto& extension) {
                return std::strcmp(extension.extensionName, name) == 0;
            });
    } catch (...) {
        return false;
    }
}

bool device_extension_available(
    VkPhysicalDevice physical_device,
    const char* name) noexcept {
    try {
        std::uint32_t count = 0U;
        if (vkEnumerateDeviceExtensionProperties(
                physical_device, nullptr, &count, nullptr) != VK_SUCCESS) {
            return false;
        }
        std::vector<VkExtensionProperties> extensions(count);
        if (vkEnumerateDeviceExtensionProperties(
                physical_device, nullptr, &count, extensions.data()) != VK_SUCCESS) {
            return false;
        }
        return std::any_of(
            extensions.begin(), extensions.end(),
            [name](const auto& extension) {
                return std::strcmp(extension.extensionName, name) == 0;
            });
    } catch (...) {
        return false;
    }
}

const char* platform_surface_extension(NativeWindowSystem system) noexcept {
#if defined(ZEVRYON_VULKAN_WSI_HAS_WIN32)
    if (system == NativeWindowSystem::Win32) {
        return VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
    }
#endif
#if defined(ZEVRYON_VULKAN_WSI_HAS_XCB)
    if (system == NativeWindowSystem::Xcb) {
        return VK_KHR_XCB_SURFACE_EXTENSION_NAME;
    }
#endif
#if defined(ZEVRYON_VULKAN_WSI_HAS_WAYLAND)
    if (system == NativeWindowSystem::Wayland) {
        return VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
    }
#endif
    return nullptr;
}

bool window_handle_valid(const NativeWindowSurfaceHandle& window) noexcept {
    if (window.generation == 0U || window.window_or_layer == 0U) {
        return false;
    }
    switch (window.system) {
#if defined(ZEVRYON_VULKAN_WSI_HAS_WIN32)
        case NativeWindowSystem::Win32:
            return window.display_or_instance != 0U;
#endif
#if defined(ZEVRYON_VULKAN_WSI_HAS_XCB)
        case NativeWindowSystem::Xcb:
            return window.display_or_instance != 0U;
#endif
#if defined(ZEVRYON_VULKAN_WSI_HAS_WAYLAND)
        case NativeWindowSystem::Wayland:
            return window.display_or_instance != 0U;
#endif
        default:
            return false;
    }
}

VkResult create_surface(
    VkInstance instance,
    const NativeWindowSurfaceHandle& window,
    VkSurfaceKHR* surface) noexcept {
    if (surface == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *surface = VK_NULL_HANDLE;
#if defined(ZEVRYON_VULKAN_WSI_HAS_WIN32)
    if (window.system == NativeWindowSystem::Win32) {
        VkWin32SurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        info.hinstance = reinterpret_cast<HINSTANCE>(
            static_cast<std::uintptr_t>(window.display_or_instance));
        info.hwnd = reinterpret_cast<HWND>(
            static_cast<std::uintptr_t>(window.window_or_layer));
        return vkCreateWin32SurfaceKHR(instance, &info, nullptr, surface);
    }
#endif
#if defined(ZEVRYON_VULKAN_WSI_HAS_XCB)
    if (window.system == NativeWindowSystem::Xcb) {
        VkXcbSurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
        info.connection = reinterpret_cast<xcb_connection_t*>(
            static_cast<std::uintptr_t>(window.display_or_instance));
        info.window = static_cast<xcb_window_t>(window.window_or_layer);
        return vkCreateXcbSurfaceKHR(instance, &info, nullptr, surface);
    }
#endif
#if defined(ZEVRYON_VULKAN_WSI_HAS_WAYLAND)
    if (window.system == NativeWindowSystem::Wayland) {
        VkWaylandSurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        info.display = reinterpret_cast<wl_display*>(
            static_cast<std::uintptr_t>(window.display_or_instance));
        info.surface = reinterpret_cast<wl_surface*>(
            static_cast<std::uintptr_t>(window.window_or_layer));
        return vkCreateWaylandSurfaceKHR(instance, &info, nullptr, surface);
    }
#endif
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

class VulkanWsiNativeGpuSdkApi final : public NativeGpuSdkApi {
public:
    VulkanWsiNativeGpuSdkApi() noexcept {
        snapshot_.probe.api_kind = NativeGpuApiKind::Vulkan;
        snapshot_.probe.availability = NativeGpuSdkAvailability::CompileOnly;
        snapshot_.probe.api_major = 1U;
        snapshot_.probe.flags = kNativeGpuSdkWindowSurface;
        std::uint32_t version = VK_API_VERSION_1_0;
#if defined(VK_VERSION_1_1)
        if (vkEnumerateInstanceVersion(&version) != VK_SUCCESS) {
            version = VK_API_VERSION_1_0;
        }
#endif
        snapshot_.probe.api_major = static_cast<std::uint16_t>(VK_API_VERSION_MAJOR(version));
        snapshot_.probe.api_minor = static_cast<std::uint16_t>(VK_API_VERSION_MINOR(version));
        snapshot_.probe.api_patch = static_cast<std::uint16_t>(VK_API_VERSION_PATCH(version));
    }

    ~VulkanWsiNativeGpuSdkApi() override { shutdown(); }

    NativeGpuApiKind kind() const noexcept override { return NativeGpuApiKind::Vulkan; }

    NativeGpuSdkProbe probe() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_.probe;
    }

    bool initialize(
        const NativeGpuSdkConfig& config,
        NativeGpuSdkError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        try {
            return initialize_locked(config, error);
        } catch (const std::bad_alloc&) {
            shutdown_locked();
            return fail(error, NativeGpuSdkErrorKind::ResourceAllocationFailed,
                        "Vulkan WSI initialization allocation failed");
        } catch (...) {
            shutdown_locked();
            return fail(error, NativeGpuSdkErrorKind::AggregateOverflow,
                        "unexpected Vulkan WSI initialization failure");
        }
    }

private:
    bool initialize_locked(
        const NativeGpuSdkConfig& config,
        NativeGpuSdkError* error) {
        if (config.api_kind != NativeGpuApiKind::Vulkan ||
            config.device_generation == 0U || config.runtime_generation == 0U ||
            !window_handle_valid(config.window)) {
            return fail(error, NativeGpuSdkErrorKind::InvalidInput,
                        "invalid Vulkan WSI SDK configuration");
        }
        const char* platform_extension = platform_surface_extension(config.window.system);
        if (platform_extension == nullptr ||
            !instance_extension_available(VK_KHR_SURFACE_EXTENSION_NAME) ||
            !instance_extension_available(platform_extension)) {
            return fail(error, NativeGpuSdkErrorKind::RuntimeUnavailable,
                        "required Vulkan surface extension is unavailable",
                        VK_ERROR_EXTENSION_NOT_PRESENT);
        }
        shutdown_locked();

        std::unique_ptr<
            VulkanWsiSharedContext,
            void (*)(VulkanWsiSharedContext*)> staged_guard(
                new (std::nothrow) VulkanWsiSharedContext(),
                &VulkanWsiNativeGpuSdkApi::destroy_staged);
        VulkanWsiSharedContext* staged = staged_guard.get();
        if (staged == nullptr) {
            return fail(error, NativeGpuSdkErrorKind::ResourceAllocationFailed,
                        "Vulkan WSI shared context allocation failed");
        }
        staged->window = config.window;
        staged->device_generation = config.device_generation;
        staged->runtime_generation = config.runtime_generation;

        const std::array<const char*, 2U> instance_extensions{
            VK_KHR_SURFACE_EXTENSION_NAME,
            platform_extension};
        VkApplicationInfo application_info{};
        application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application_info.pApplicationName = "Zevryon";
        application_info.applicationVersion = 1U;
        application_info.pEngineName = "Zevryon-Z2F8B2B";
        application_info.engineVersion = 1U;
        application_info.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo instance_info{};
        instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance_info.pApplicationInfo = &application_info;
        instance_info.enabledExtensionCount =
            static_cast<std::uint32_t>(instance_extensions.size());
        instance_info.ppEnabledExtensionNames = instance_extensions.data();
        VkResult result = vkCreateInstance(&instance_info, nullptr, &staged->instance);
        if (result != VK_SUCCESS) {
            delete staged;
            return fail(error, NativeGpuSdkErrorKind::RuntimeUnavailable,
                        "vkCreateInstance for WSI failed", result);
        }
        result = create_surface(staged->instance, config.window, &staged->surface);
        if (result != VK_SUCCESS) {
            vkDestroyInstance(staged->instance, nullptr);
            delete staged;
            return fail(error, NativeGpuSdkErrorKind::SurfaceConfigurationFailed,
                        "Vulkan platform surface creation failed", result);
        }

        std::uint32_t physical_count = 0U;
        result = vkEnumeratePhysicalDevices(staged->instance, &physical_count, nullptr);
        if (result != VK_SUCCESS || physical_count == 0U) {
            staged_guard.reset();
            return fail(error, NativeGpuSdkErrorKind::DeviceCreationFailed,
                        "no Vulkan physical device is available for WSI", result);
        }
        std::vector<VkPhysicalDevice> physical_devices(physical_count);
        result = vkEnumeratePhysicalDevices(
            staged->instance, &physical_count, physical_devices.data());
        if (result != VK_SUCCESS) {
            staged_guard.reset();
            return fail(error, NativeGpuSdkErrorKind::DeviceCreationFailed,
                        "vkEnumeratePhysicalDevices failed", result);
        }

        bool found = false;
        std::uint32_t selected_rank = 0U;
        VkPhysicalDeviceProperties selected_properties{};
        for (VkPhysicalDevice candidate : physical_devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU &&
                (config.allow_software_device == 0U ||
                 config.require_real_device != 0U)) {
                continue;
            }
            if (!device_extension_available(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
                continue;
            }
            std::uint32_t queue_count = 0U;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, nullptr);
            std::vector<VkQueueFamilyProperties> queues(queue_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, queues.data());
            std::uint32_t graphics = std::numeric_limits<std::uint32_t>::max();
            std::uint32_t present = std::numeric_limits<std::uint32_t>::max();
            for (std::uint32_t index = 0U; index < queue_count; ++index) {
                if ((queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U &&
                    graphics == std::numeric_limits<std::uint32_t>::max()) {
                    graphics = index;
                }
                VkBool32 supported = VK_FALSE;
                if (vkGetPhysicalDeviceSurfaceSupportKHR(
                        candidate, index, staged->surface, &supported) == VK_SUCCESS &&
                    supported == VK_TRUE) {
                    present = index;
                    if ((queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
                        graphics = index;
                        break;
                    }
                }
            }
            if (graphics == std::numeric_limits<std::uint32_t>::max() ||
                present == std::numeric_limits<std::uint32_t>::max()) {
                continue;
            }
            const std::uint32_t rank =
                properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 1U :
                (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                    ? 3U : 2U);
            if (found && rank <= selected_rank) {
                continue;
            }
            staged->physical_device = candidate;
            staged->graphics_queue_family = graphics;
            staged->present_queue_family = present;
#if defined(VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME)
            staged->incremental_present = device_extension_available(
                candidate, VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME) ? 1U : 0U;
#else
            staged->incremental_present = 0U;
#endif
            selected_properties = properties;
            selected_rank = rank;
            found = true;
            if (rank == 3U) {
                break;
            }
        }
        if (!found) {
            staged_guard.reset();
            return fail(error, NativeGpuSdkErrorKind::QueueCreationFailed,
                        "no Vulkan device exposes graphics, present, and swapchain support");
        }

        const float priority = 1.0F;
        std::array<VkDeviceQueueCreateInfo, 2U> queue_infos{};
        std::uint32_t queue_info_count = 1U;
        queue_infos[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_infos[0].queueFamilyIndex = staged->graphics_queue_family;
        queue_infos[0].queueCount = 1U;
        queue_infos[0].pQueuePriorities = &priority;
        if (staged->present_queue_family != staged->graphics_queue_family) {
            queue_info_count = 2U;
            queue_infos[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_infos[1].queueFamilyIndex = staged->present_queue_family;
            queue_infos[1].queueCount = 1U;
            queue_infos[1].pQueuePriorities = &priority;
        }
        std::array<const char*, 2U> device_extensions{
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
#if defined(VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME)
            VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME
#else
            VK_KHR_SWAPCHAIN_EXTENSION_NAME
#endif
        };
        const std::uint32_t device_extension_count =
            staged->incremental_present != 0U ? 2U : 1U;
        VkDeviceCreateInfo device_info{};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.queueCreateInfoCount = queue_info_count;
        device_info.pQueueCreateInfos = queue_infos.data();
        device_info.enabledExtensionCount = device_extension_count;
        device_info.ppEnabledExtensionNames = device_extensions.data();
        result = vkCreateDevice(
            staged->physical_device, &device_info, nullptr, &staged->device);
        if (result != VK_SUCCESS) {
            staged_guard.reset();
            return fail(error, NativeGpuSdkErrorKind::DeviceCreationFailed,
                        "vkCreateDevice with swapchain support failed", result);
        }
        vkGetDeviceQueue(
            staged->device, staged->graphics_queue_family, 0U,
            &staged->graphics_queue);
        vkGetDeviceQueue(
            staged->device, staged->present_queue_family, 0U,
            &staged->present_queue);

        staged->software_device =
            selected_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 1U : 0U;
        context_ = staged_guard.release();
        config_ = config;
        snapshot_.config = config;
        snapshot_.probe.availability = NativeGpuSdkAvailability::RuntimeReady;
        snapshot_.probe.flags = kNativeGpuSdkRealDevice |
                                kNativeGpuSdkWindowSurface;
        if (staged->software_device != 0U) {
            snapshot_.probe.flags |= kNativeGpuSdkSoftwareDevice;
        }
        snapshot_.probe.vendor_id = selected_properties.vendorID;
        snapshot_.probe.device_id = selected_properties.deviceID;
        snapshot_.probe.queue_family_index = staged->graphics_queue_family;
        snapshot_.probe.runtime_generation = config.runtime_generation;
        std::uint64_t checksum = kFnvOffset;
        hash_value(&checksum, selected_properties.vendorID);
        hash_value(&checksum, selected_properties.deviceID);
        hash_value(&checksum, staged->graphics_queue_family);
        hash_value(&checksum, staged->present_queue_family);
        hash_value(&checksum, config.window.system);
        hash_value(&checksum, config.runtime_generation);
        snapshot_.probe.checksum = checksum;
        snapshot_.initialized_devices += 1U;
        initialized_ = true;
        return true;
    }

public:
    bool export_context(
        NativeGpuSdkContextHandle* context,
        NativeGpuSdkError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (context == nullptr || !initialized_ || context_ == nullptr) {
            if (context != nullptr) {
                *context = {};
            }
            return fail(error, NativeGpuSdkErrorKind::RuntimeUnavailable,
                        "Vulkan WSI context is not initialized");
        }
        *context = {};
        context->api_kind = NativeGpuApiKind::Vulkan;
        context->flags = kNativeGpuSdkContextDeviceValid |
                         kNativeGpuSdkContextGraphicsQueueValid |
                         kNativeGpuSdkContextPresentQueueValid |
                         detail::kNativeGpuSdkContextRetainedLease |
                         detail::kNativeGpuSdkContextVulkanWsi;
        if (context_->graphics_queue_family == context_->present_queue_family) {
            context->flags |= kNativeGpuSdkContextSharedGraphicsPresentQueue;
        }
        if (context_->software_device != 0U) {
            context->flags |= kNativeGpuSdkContextSoftwareDevice;
        }
        context->device_generation = context_->device_generation;
        context->runtime_generation = context_->runtime_generation;
        context->instance_or_factory = opaque_handle_id(context_);
        context->physical_device_or_adapter =
            opaque_handle_id(context_->physical_device);
        context->device = opaque_handle_id(context_->device);
        context->graphics_queue =
            opaque_handle_id(context_->graphics_queue);
        context->present_queue =
            opaque_handle_id(context_->present_queue);
        context->graphics_queue_family = context_->graphics_queue_family;
        context->present_queue_family = context_->present_queue_family;
        return true;
    }

    bool configure_offscreen_surface(
        const GpuSurfaceDescriptor&,
        std::uint32_t,
        NativeGpuSdkError* error) noexcept override {
        clear_error(error);
        return fail(error, NativeGpuSdkErrorKind::UnsupportedBackend,
                    "Vulkan WSI owner does not allocate a parallel offscreen image ring");
    }

    bool acquire_image(
        const GpuSurfaceDescriptor&,
        std::uint64_t,
        NativePlatformSwapchainImage*,
        NativeAcquireStatus*,
        NativeGpuSdkError* error) noexcept override {
        clear_error(error);
        return fail(error, NativeGpuSdkErrorKind::UnsupportedBackend,
                    "window images are acquired through NativeWindowSwapchainApi");
    }

    bool execute_submission(
        const NativePlatformSubmission&,
        NativeGpuSdkSubmissionReceipt*,
        NativeGpuSdkError* error) noexcept override {
        clear_error(error);
        return fail(error, NativeGpuSdkErrorKind::UnsupportedBackend,
                    "window submissions are executed through NativeWindowSwapchainApi");
    }

    bool retire_completed(
        std::uint64_t completed_fence_value,
        NativeGpuSdkError* error) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_error(error);
        if (completed_fence_value < snapshot_.completed_fence_value ||
            completed_fence_value > snapshot_.last_submitted_fence_value) {
            return fail(error, NativeGpuSdkErrorKind::FenceRegression,
                        "Vulkan WSI owner completion fence is invalid");
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
    static void destroy_staged(VulkanWsiSharedContext* context) noexcept {
        if (context == nullptr) {
            return;
        }
        if (context->device != VK_NULL_HANDLE) {
            vkDestroyDevice(context->device, nullptr);
        }
        if (context->surface != VK_NULL_HANDLE && context->instance != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(context->instance, context->surface, nullptr);
        }
        if (context->instance != VK_NULL_HANDLE) {
            vkDestroyInstance(context->instance, nullptr);
        }
        delete context;
    }

    void shutdown_locked() noexcept {
        if (context_ != nullptr) {
            detail::release_vulkan_wsi_owner(context_);
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
    NativeGpuSdkConfig config_;
    VulkanWsiSharedContext* context_{nullptr};
    bool initialized_{false};
};

} // namespace

namespace detail {

VulkanWsiSharedContext* retain_vulkan_wsi_context(
    const NativeGpuSdkContextHandle& context) noexcept {
    if (context.api_kind != NativeGpuApiKind::Vulkan ||
        (context.flags & kNativeGpuSdkContextRetainedLease) == 0U ||
        (context.flags & kNativeGpuSdkContextVulkanWsi) == 0U ||
        context.instance_or_factory == 0U) {
        return nullptr;
    }
    auto* shared = reinterpret_cast<VulkanWsiSharedContext*>(
        static_cast<std::uintptr_t>(context.instance_or_factory));
    if (shared->owner_released.load(std::memory_order_acquire) != 0U) {
        return nullptr;
    }
    std::uint32_t current = shared->references.load(std::memory_order_acquire);
    while (current != 0U && current != std::numeric_limits<std::uint32_t>::max()) {
        if (shared->references.compare_exchange_weak(
                current, current + 1U,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            if (shared->owner_released.load(std::memory_order_acquire) == 0U) {
                return shared;
            }
            release_vulkan_wsi_context(shared);
            return nullptr;
        }
    }
    return nullptr;
}

void release_vulkan_wsi_context(VulkanWsiSharedContext* context) noexcept {
    if (context == nullptr) {
        return;
    }
    if (context->references.fetch_sub(1U, std::memory_order_acq_rel) != 1U) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(context->device_mutex);
        if (context->device != VK_NULL_HANDLE) {
            (void)vkDeviceWaitIdle(context->device);
            vkDestroyDevice(context->device, nullptr);
        }
        if (context->surface != VK_NULL_HANDLE && context->instance != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(context->instance, context->surface, nullptr);
        }
        if (context->instance != VK_NULL_HANDLE) {
            vkDestroyInstance(context->instance, nullptr);
        }
        context->device = VK_NULL_HANDLE;
        context->surface = VK_NULL_HANDLE;
        context->instance = VK_NULL_HANDLE;
    }
    delete context;
}

void release_vulkan_wsi_owner(VulkanWsiSharedContext* context) noexcept {
    if (context != nullptr) {
        context->owner_released.store(1U, std::memory_order_release);
    }
    release_vulkan_wsi_context(context);
}

} // namespace detail

std::unique_ptr<NativeGpuSdkApi> make_vulkan_wsi_native_gpu_sdk_api() noexcept {
    try {
        return std::make_unique<VulkanWsiNativeGpuSdkApi>();
    } catch (...) {
        return nullptr;
    }
}


} // namespace zevryon::text

#endif // ZEVRYON_HAS_VULKAN_WSI
