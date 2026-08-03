#include "native_shader_surface.hpp"
#include "native_shader_execution.hpp"

namespace zevryon::text {

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
