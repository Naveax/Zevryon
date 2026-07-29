#pragma once

#include "native_gpu_sdk_execution.hpp"

#include <cstdint>

namespace zevryon::text {

enum NativeShaderSurfaceFlags : std::uint32_t {
    kNativeShaderSurfaceReady = 1U << 0U,
    kNativeShaderSurfaceNonOwning = 1U << 1U,
    kNativeShaderSurfacePremultipliedAlpha = 1U << 2U
};

enum class NativeShaderSurfaceState : std::uint8_t {
    Undefined = 0U,
    ShaderRead
};

// A non-owning view of one completed native shader output. The producer keeps
// the underlying resource alive; native presenters retain it for their own
// in-flight lifetime before submitting work on the same device queue.
struct NativeShaderSurfaceView final {
    NativeGpuApiKind api_kind{NativeGpuApiKind::ReferenceCpu};
    GpuSurfaceFormat format{GpuSurfaceFormat::Bgra8Unorm};
    NativeShaderSurfaceState state{NativeShaderSurfaceState::Undefined};
    std::uint8_t reserved0{0U};
    std::uint32_t flags{0U};
    std::uint64_t device_generation{0U};
    std::uint64_t runtime_generation{0U};
    std::uint64_t executor_generation{0U};
    std::uint64_t output_generation{0U};
    std::uint64_t frame_id{0U};
    std::uint64_t content_checksum{0U};
    std::uint64_t native_resource{0U};
    std::uint32_t width{0U};
    std::uint32_t height{0U};

    bool empty() const noexcept {
        return (flags & kNativeShaderSurfaceReady) == 0U;
    }
};
static_assert(sizeof(NativeShaderSurfaceView) == 72U);

bool native_shader_surface_view_valid(
    const NativeShaderSurfaceView& view) noexcept;

} // namespace zevryon::text
