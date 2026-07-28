#pragma once

#include "native_gpu_sdk_execution.hpp"

#include <cstdint>
#include <memory>

namespace zevryon::text::test {

class MetalWindowTestHost {
public:
    virtual ~MetalWindowTestHost() = default;

    virtual NativeWindowSurfaceHandle handle() const noexcept = 0;
    virtual bool resize(std::uint32_t width, std::uint32_t height) noexcept = 0;
    virtual void set_visible(bool visible) noexcept = 0;
    virtual void pump_events() noexcept = 0;
};

std::unique_ptr<MetalWindowTestHost>
make_metal_window_test_host(
    std::uint32_t width,
    std::uint32_t height) noexcept;

} // namespace zevryon::text::test
