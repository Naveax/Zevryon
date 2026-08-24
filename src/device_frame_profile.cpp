#include "device_frame_profile.hpp"

namespace zevryon::massivedoc {

bool DeviceFrameBudgetProfile::valid() const noexcept {
    return minimum_physical_ram_mib > 0U && frame_budget.valid() &&
           prefetch_reserve_us > 0U &&
           prefetch_reserve_us <= frame_budget.prefetch_budget_us;
}

DeviceFrameProfile select_device_frame_profile(std::uint64_t total_ram_mib) noexcept {
    if (total_ram_mib < 3072U) {
        return DeviceFrameProfile::LegacyPhone;
    }
    if (total_ram_mib < 6144U) {
        return DeviceFrameProfile::MidPhone;
    }
    if (total_ram_mib < 12'288U) {
        return DeviceFrameProfile::ModernPhone;
    }
    return DeviceFrameProfile::Desktop;
}

DeviceFrameBudgetProfile device_frame_budget_profile(DeviceFrameProfile profile) noexcept {
    switch (profile) {
    case DeviceFrameProfile::LegacyPhone:
        return {profile, 2048U, {33'300U, 4'000U, 1'000U, 500U}, 200U};
    case DeviceFrameProfile::MidPhone:
        return {profile, 4096U, {16'600U, 2'000U, 500U, 250U}, 200U};
    case DeviceFrameProfile::ModernPhone:
        return {profile, 8192U, {11'100U, 1'250U, 300U, 150U}, 150U};
    case DeviceFrameProfile::Desktop:
        return {profile, 8192U, {8'330U, 1'000U, 250U, 125U}, 125U};
    }
    return {};
}

const char* device_frame_profile_name(DeviceFrameProfile profile) noexcept {
    switch (profile) {
    case DeviceFrameProfile::LegacyPhone:
        return "legacy-phone";
    case DeviceFrameProfile::MidPhone:
        return "mid-phone";
    case DeviceFrameProfile::ModernPhone:
        return "modern-phone";
    case DeviceFrameProfile::Desktop:
        return "desktop";
    }
    return "unknown";
}

} // namespace zevryon::massivedoc
