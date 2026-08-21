#include "device_frame_profile.hpp"
#include "frame_budget_scheduler.hpp"

#include <array>
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

void certify_profile(DeviceFrameProfile profile) {
    const DeviceFrameBudgetProfile device = device_frame_budget_profile(profile);
    require(device.valid(), "device frame profile invalid");

    FrameBudgetScheduler scheduler(device.frame_budget);
    require(scheduler.valid(), "profile scheduler invalid");
    const PrefetchTicket ticket = scheduler.update_scroll_motion(4096);
    require(ticket.direction == 1, "profile scheduler missing forward ticket");

    scheduler.begin_frame(FramePressure::Normal, FrameVisibility::Visible);
    FrameWorkRequest prefetch;
    prefetch.work_class = FrameWorkClass::Prefetch;
    prefetch.lane = FrameExecutionLane::Worker;
    prefetch.reserve_us = device.prefetch_reserve_us;
    prefetch.may_block = true;
    prefetch.prefetch_ticket = ticket;
    require(scheduler.reserve(prefetch) == FrameAdmission::VisiblePhaseOpen,
            "prefetch ran before visible phase completed");

    FrameWorkRequest visible;
    visible.work_class = FrameWorkClass::Visible;
    visible.lane = FrameExecutionLane::Ui;
    visible.reserve_us = 1U;
    visible.may_block = false;
    require(scheduler.reserve(visible) == FrameAdmission::Admitted,
            "visible work rejected by profile budget");
    scheduler.finish_visible_phase();
    require(scheduler.reserve(prefetch) == FrameAdmission::Admitted,
            "post-visible prefetch rejected within profile reserve");

    scheduler.begin_frame(FramePressure::Elevated, FrameVisibility::Visible);
    require(scheduler.reserve(visible) == FrameAdmission::Admitted,
            "elevated visible work rejected");
    scheduler.finish_visible_phase();
    FrameWorkRequest elevated_prefetch = prefetch;
    elevated_prefetch.reserve_us = device.frame_budget.prefetch_budget_us / 2U + 1U;
    require(scheduler.reserve(elevated_prefetch) == FrameAdmission::ClassBudgetExhausted,
            "elevated pressure did not halve optional prefetch cap");

    scheduler.begin_frame(FramePressure::Critical, FrameVisibility::Visible);
    require(scheduler.reserve(visible) == FrameAdmission::Admitted,
            "critical pressure rejected visible work");
    scheduler.finish_visible_phase();
    FrameWorkRequest critical_prefetch = prefetch;
    critical_prefetch.reserve_us = 1U;
    require(scheduler.reserve(critical_prefetch) == FrameAdmission::SuppressedByPressure,
            "critical pressure admitted speculative work");

    scheduler.begin_frame(FramePressure::Normal, FrameVisibility::Hidden);
    require(scheduler.reserve(visible) == FrameAdmission::SuppressedByVisibility,
            "hidden profile admitted frame work");
    require(scheduler.snapshot().remaining_us == 0U,
            "hidden profile exposed non-zero frame budget");
}

} // namespace

int main() {
    constexpr std::array<DeviceFrameProfile, 4> profiles{
        DeviceFrameProfile::LegacyPhone,
        DeviceFrameProfile::MidPhone,
        DeviceFrameProfile::ModernPhone,
        DeviceFrameProfile::Desktop,
    };
    for (const DeviceFrameProfile profile : profiles) {
        certify_profile(profile);
    }
    std::cout << "Zevryon frame-profile scheduler certification passed\n";
    return 0;
}
