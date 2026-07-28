#pragma once

#include "native_gpu_sdk_execution.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>

#if defined(__OBJC__)
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#endif

namespace zevryon::text::detail {

constexpr std::uint32_t kNativeGpuSdkContextRetainedLease = 1U << 5U;
constexpr std::uint32_t kNativeGpuSdkContextMetalWindow = 1U << 7U;

#if defined(__OBJC__)
struct MetalWindowSharedContext final {
    std::atomic<std::uint32_t> references{1U};
    std::mutex device_mutex;
    NativeWindowSurfaceHandle window;
    id<MTLDevice> device{nil};
    id<MTLCommandQueue> queue{nil};
    CAMetalLayer* layer{nil};
    std::uint64_t device_generation{0U};
    std::uint64_t runtime_generation{0U};
    std::atomic<std::uint8_t> owner_released{0U};
    std::uint8_t reserved[7]{0, 0, 0, 0, 0, 0, 0};
};

MetalWindowSharedContext* retain_metal_window_context(
    const NativeGpuSdkContextHandle& context) noexcept;
void release_metal_window_context(MetalWindowSharedContext* context) noexcept;
void release_metal_window_owner(MetalWindowSharedContext* context) noexcept;
#endif

} // namespace zevryon::text::detail
