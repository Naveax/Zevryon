#pragma once

#include "native_gpu_sdk_execution.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace zevryon::text::test {

class NativeVulkanTestWindow final {
public:
    struct State;

    NativeVulkanTestWindow() noexcept;
    ~NativeVulkanTestWindow();

    NativeVulkanTestWindow(const NativeVulkanTestWindow&) = delete;
    NativeVulkanTestWindow& operator=(const NativeVulkanTestWindow&) = delete;

    bool create(
        NativeWindowSystem system,
        std::uint32_t width,
        std::uint32_t height,
        std::string* error) noexcept;
    bool resize(
        std::uint32_t width,
        std::uint32_t height,
        std::string* error) noexcept;
    bool pump(std::string* error) noexcept;
    void destroy() noexcept;

    NativeWindowSurfaceHandle handle() const noexcept;
    std::uint32_t width() const noexcept;
    std::uint32_t height() const noexcept;

private:
    std::unique_ptr<State> state_;
};

} // namespace zevryon::text::test
