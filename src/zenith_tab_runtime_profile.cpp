#include "zenith_tab_runtime_profile.hpp"

namespace zevryon::massivedoc {

ZenithTabRuntimeConfig make_zenith_tab_runtime_config(
    DeviceFrameProfile profile,
    LayoutConfig layout) noexcept {
    const DeviceFrameBudgetProfile device = device_frame_budget_profile(profile);
    ZenithTabRuntimeConfig config;
    config.layout = layout;
    config.frame_budget = device.frame_budget;
    config.prefetch_reserve_us = device.prefetch_reserve_us;
    return config;
}

} // namespace zevryon::massivedoc
