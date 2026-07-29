#include "native_shader_surface.hpp"
#include "native_shader_execution.hpp"

namespace zevryon::text {

// Validate only the cross-backend ownership and generation contract here;
// backend-specific resource-state validation remains in each native presenter.
bool native_shader_surface_view_valid(
    const NativeShaderSurfaceView& view) noexcept {
    const std::uint32_t required_flags =
        kNativeShaderSurfaceReady |
        kNativeShaderSurfaceNonOwning |
        kNativeShaderSurfacePremultipliedAlpha;
    return view.api_kind != NativeGpuApiKind::ReferenceCpu &&
        view.format == GpuSurfaceFormat::Bgra8Unorm &&
        view.state == NativeShaderSurfaceState::ShaderRead &&
        (view.flags & required_flags) == required_flags &&
        view.device_generation != 0U &&
        view.runtime_generation != 0U &&
        view.executor_generation != 0U &&
        view.output_generation != 0U &&
        view.frame_id != 0U &&
        view.content_checksum != 0U &&
        view.native_resource != 0U &&
        view.width != 0U &&
        view.height != 0U;
}

bool NativeShaderExecutor::export_surface(
    NativeShaderSurfaceView* surface,
    NativeShaderExecutionError* error) noexcept {
    if (surface != nullptr) {
        *surface = {};
    }
    if (error != nullptr) {
        error->kind = NativeShaderExecutionErrorKind::UnsupportedBackend;
        error->native_code = 0;
        try {
            error->message = "native shader surface export is unavailable";
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

} // namespace zevryon::text
