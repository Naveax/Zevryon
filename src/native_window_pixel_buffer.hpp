#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace zevryon::text {

enum class GpuSurfaceFormat : std::uint8_t;
struct GpuSurfaceDescriptor;

struct NativeWindowPixelBufferView final {
    std::span<const std::byte> bytes;
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t row_bytes{0};
    GpuSurfaceFormat format{static_cast<GpuSurfaceFormat>(0)};
    std::uint8_t premultiplied_alpha{1};
    std::uint8_t reserved[3]{0, 0, 0};
    std::uint64_t checksum{0};

    bool empty() const noexcept { return bytes.empty(); }
};

bool native_window_pixel_buffer_valid(
    const NativeWindowPixelBufferView& view,
    const GpuSurfaceDescriptor& surface) noexcept;

} // namespace zevryon::text
