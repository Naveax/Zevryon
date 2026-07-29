#include "native_window_swapchain.hpp"

#include <limits>

namespace zevryon::text {

bool native_window_pixel_buffer_valid(
    const NativeWindowPixelBufferView& view,
    const GpuSurfaceDescriptor& surface) noexcept {
    if (view.empty()) {
        return true;
    }
    const std::uint64_t width = view.width;
    if (width > (std::numeric_limits<std::uint32_t>::max)() / 4U) {
        return false;
    }
    const std::uint32_t minimum_row_bytes = view.width * 4U;
    if (view.width != surface.width ||
        view.height != surface.height ||
        view.format != surface.format ||
        view.premultiplied_alpha != surface.premultiplied_alpha ||
        view.row_bytes < minimum_row_bytes ||
        (view.row_bytes % 4U) != 0U) {
        return false;
    }
    const std::uint64_t row_bytes = view.row_bytes;
    const std::uint64_t height = view.height;
    if (height != 0U &&
        row_bytes > (std::numeric_limits<std::uint64_t>::max)() / height) {
        return false;
    }
    return row_bytes * height == view.bytes.size();
}

} // namespace zevryon::text
