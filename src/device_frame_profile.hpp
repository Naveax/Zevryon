#pragma once

#include "frame_budget_scheduler.hpp"

#include <cstdint>

namespace zevryon::massivedoc {

enum class DeviceFrameProfile : std::uint8_t {
    LegacyPhone = 0U,
    MidPhone,
    ModernPhone,
    Desktop,
};

struct DeviceFrameBudgetProfile {
    DeviceFrameProfile profile{DeviceFrameProfile::MidPhone};
    std::uint32_t minimum_physical_ram_mib{4096U};
    FrameBudgetPolicy frame_budget{};
    std::uint32_t prefetch_reserve_us{200U};

    bool valid() const noexcept;
};

DeviceFrameProfile select_device_frame_profile(std::uint64_t total_ram_mib) noexcept;
DeviceFrameBudgetProfile device_frame_budget_profile(DeviceFrameProfile profile) noexcept;
const char* device_frame_profile_name(DeviceFrameProfile profile) noexcept;

} // namespace zevryon::massivedoc
