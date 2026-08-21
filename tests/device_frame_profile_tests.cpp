#include "device_frame_profile.hpp"
#include "zenith_tab_runtime_profile.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
using namespace zevryon::massivedoc;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool value, std::string_view message) {
    if (!value) {
        fail(message);
    }
}

void test_ram_threshold_parity() {
    require(select_device_frame_profile(2048U) == DeviceFrameProfile::LegacyPhone,
            "2 GiB device did not select legacy-phone");
    require(select_device_frame_profile(3071U) == DeviceFrameProfile::LegacyPhone,
            "legacy upper threshold mismatch");
    require(select_device_frame_profile(3072U) == DeviceFrameProfile::MidPhone,
            "mid-phone lower threshold mismatch");
    require(select_device_frame_profile(6143U) == DeviceFrameProfile::MidPhone,
            "mid-phone upper threshold mismatch");
    require(select_device_frame_profile(6144U) == DeviceFrameProfile::ModernPhone,
            "modern-phone lower threshold mismatch");
    require(select_device_frame_profile(12'287U) == DeviceFrameProfile::ModernPhone,
            "modern-phone upper threshold mismatch");
    require(select_device_frame_profile(12'288U) == DeviceFrameProfile::Desktop,
            "desktop lower threshold mismatch");
}

void test_profile_budgets_match_performance_contract() {
    const auto legacy = device_frame_budget_profile(DeviceFrameProfile::LegacyPhone);
    const auto mid = device_frame_budget_profile(DeviceFrameProfile::MidPhone);
    const auto modern = device_frame_budget_profile(DeviceFrameProfile::ModernPhone);
    const auto desktop = device_frame_budget_profile(DeviceFrameProfile::Desktop);

    require(legacy.valid() && legacy.frame_budget.frame_budget_us == 33'300U,
            "legacy frame budget mismatch");
    require(mid.valid() && mid.frame_budget.frame_budget_us == 16'600U,
            "mid frame budget mismatch");
    require(modern.valid() && modern.frame_budget.frame_budget_us == 11'100U,
            "modern frame budget mismatch");
    require(desktop.valid() && desktop.frame_budget.frame_budget_us == 8'330U,
            "desktop frame budget mismatch");

    require(std::string_view(device_frame_profile_name(DeviceFrameProfile::LegacyPhone)) ==
                "legacy-phone",
            "legacy profile name drifted");
    require(std::string_view(device_frame_profile_name(DeviceFrameProfile::Desktop)) ==
                "desktop",
            "desktop profile name drifted");
}

void test_runtime_factory_applies_profile() {
    LayoutConfig layout;
    layout.max_source_window_cache_bytes = 256U * 1024U;
    const ZenithTabRuntimeConfig config = make_zenith_tab_runtime_config(
        DeviceFrameProfile::Desktop,
        layout);
    require(config.valid(), "desktop runtime factory produced invalid config");
    require(config.frame_budget.frame_budget_us == 8'330U,
            "runtime factory did not apply desktop frame budget");
    require(config.prefetch_reserve_us == 125U,
            "runtime factory did not apply desktop prefetch reserve");
    require(config.layout.max_source_window_cache_bytes == 256U * 1024U,
            "runtime factory lost caller layout policy");
}

} // namespace

int main() {
    test_ram_threshold_parity();
    test_profile_budgets_match_performance_contract();
    test_runtime_factory_applies_profile();
    std::cout << "Zevryon device frame-profile tests passed\n";
    return 0;
}
