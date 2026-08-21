#include "frame_budget_scheduler.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
using namespace zevryon::massivedoc;

[[noreturn]] void die(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool value, std::string_view message) {
    if (!value) {
        die(message);
    }
}

FrameWorkRequest request(
    FrameWorkClass work_class,
    FrameExecutionLane lane,
    std::uint32_t reserve_us,
    bool may_block = false,
    PrefetchTicket ticket = {}) {
    FrameWorkRequest result;
    result.work_class = work_class;
    result.lane = lane;
    result.reserve_us = reserve_us;
    result.may_block = may_block;
    result.prefetch_ticket = ticket;
    return result;
}

FrameBudgetPolicy policy() {
    return FrameBudgetPolicy{1000U, 400U, 300U, 200U};
}

void test_policy_validation() {
    require(policy().valid(), "baseline policy must be valid");
    require(!FrameBudgetPolicy{}.valid(), "zero frame budget must be invalid");
    require(
        !FrameBudgetPolicy{100U, 101U, 0U, 0U}.valid(),
        "prefetch cap above frame budget must be invalid");
}

void test_visible_first_and_leftover_budget() {
    FrameBudgetScheduler scheduler(policy());
    require(scheduler.valid(), "scheduler policy invalid");
    const PrefetchTicket ticket = scheduler.update_scroll_motion(256);
    scheduler.begin_frame();

    require(
        scheduler.reserve(request(
            FrameWorkClass::Background,
            FrameExecutionLane::Worker,
            100U)) == FrameAdmission::VisiblePhaseOpen,
        "background work admitted before visible completion");
    require(
        scheduler.reserve(request(
            FrameWorkClass::Visible,
            FrameExecutionLane::Ui,
            850U)) == FrameAdmission::Admitted,
        "visible work rejected");
    scheduler.finish_visible_phase();

    require(
        scheduler.reserve(request(
            FrameWorkClass::Prefetch,
            FrameExecutionLane::Worker,
            151U,
            true,
            ticket)) == FrameAdmission::FrameBudgetExhausted,
        "prefetch exceeded leftover frame budget");
    require(
        scheduler.reserve(request(
            FrameWorkClass::Prefetch,
            FrameExecutionLane::Worker,
            150U,
            true,
            ticket)) == FrameAdmission::Admitted,
        "prefetch did not consume exact leftover budget");

    const FrameBudgetSnapshot snapshot = scheduler.snapshot();
    require(snapshot.spent_us == 1000U, "frame spend mismatch");
    require(snapshot.remaining_us == 0U, "frame remaining mismatch");
    require(snapshot.visible_spent_us == 850U, "visible spend mismatch");
    require(snapshot.prefetch_spent_us == 150U, "prefetch spend mismatch");
    require(snapshot.admitted_requests == 2U, "admitted count mismatch");
    require(snapshot.rejected_requests == 2U, "rejected count mismatch");
}

void test_optional_class_caps() {
    FrameBudgetScheduler scheduler(policy());
    scheduler.begin_frame();
    require(
        scheduler.reserve(request(
            FrameWorkClass::Visible,
            FrameExecutionLane::Ui,
            100U)) == FrameAdmission::Admitted,
        "visible seed rejected");
    scheduler.finish_visible_phase();

    require(
        scheduler.reserve(request(
            FrameWorkClass::Background,
            FrameExecutionLane::Worker,
            300U)) == FrameAdmission::Admitted,
        "background cap reservation rejected");
    require(
        scheduler.reserve(request(
            FrameWorkClass::Background,
            FrameExecutionLane::Worker,
            1U)) == FrameAdmission::ClassBudgetExhausted,
        "background cap overrun admitted");
    require(
        scheduler.reserve(request(
            FrameWorkClass::Maintenance,
            FrameExecutionLane::Worker,
            200U)) == FrameAdmission::Admitted,
        "maintenance cap reservation rejected");
    require(
        scheduler.reserve(request(
            FrameWorkClass::Maintenance,
            FrameExecutionLane::Worker,
            1U)) == FrameAdmission::ClassBudgetExhausted,
        "maintenance cap overrun admitted");
}

void test_blocking_ui_rejected() {
    FrameBudgetScheduler scheduler(policy());
    scheduler.begin_frame();
    require(
        scheduler.reserve(request(
            FrameWorkClass::Visible,
            FrameExecutionLane::Ui,
            10U,
            true)) == FrameAdmission::BlockingOnUi,
        "blocking visible work admitted on UI lane");

    require(
        scheduler.reserve(request(
            FrameWorkClass::Visible,
            FrameExecutionLane::Ui,
            10U)) == FrameAdmission::Admitted,
        "non-blocking visible work rejected");
    scheduler.finish_visible_phase();
    require(
        scheduler.reserve(request(
            FrameWorkClass::Background,
            FrameExecutionLane::Ui,
            10U,
            true)) == FrameAdmission::BlockingOnUi,
        "blocking background work admitted on UI lane");
    require(
        scheduler.reserve(request(
            FrameWorkClass::Background,
            FrameExecutionLane::Worker,
            10U,
            true)) == FrameAdmission::Admitted,
        "blocking worker work rejected");
}

void test_prefetch_epoch_cancellation() {
    FrameBudgetScheduler scheduler(policy());
    const PrefetchTicket forward = scheduler.update_scroll_motion(1024);
    scheduler.begin_frame();
    scheduler.finish_visible_phase();
    require(
        scheduler.reserve(request(
            FrameWorkClass::Prefetch,
            FrameExecutionLane::Worker,
            10U,
            true,
            forward)) == FrameAdmission::Admitted,
        "current forward prefetch rejected");

    const PrefetchTicket reverse = scheduler.update_scroll_motion(-1024);
    require(reverse.epoch > forward.epoch, "direction reversal did not advance epoch");
    require(reverse.direction == -1, "reverse direction mismatch");
    require(
        scheduler.reserve(request(
            FrameWorkClass::Prefetch,
            FrameExecutionLane::Worker,
            10U,
            true,
            forward)) == FrameAdmission::StalePrefetch,
        "stale forward prefetch survived reversal");
    require(
        scheduler.reserve(request(
            FrameWorkClass::Prefetch,
            FrameExecutionLane::Worker,
            10U,
            true,
            reverse)) == FrameAdmission::Admitted,
        "current reverse prefetch rejected");

    const PrefetchTicket stopped = scheduler.update_scroll_motion(0);
    require(stopped.epoch > reverse.epoch, "scroll stop did not advance epoch");
    require(stopped.direction == 0, "stationary direction mismatch");
    require(
        scheduler.reserve(request(
            FrameWorkClass::Prefetch,
            FrameExecutionLane::Worker,
            10U,
            true,
            stopped)) == FrameAdmission::StalePrefetch,
        "stationary speculative prefetch admitted");
}

void test_pressure_suppression() {
    FrameBudgetScheduler scheduler(policy());
    scheduler.update_scroll_motion(256);
    scheduler.begin_frame(FramePressure::Elevated);
    scheduler.finish_visible_phase();
    const PrefetchTicket ticket = scheduler.current_prefetch_ticket();

    require(
        scheduler.reserve(request(
            FrameWorkClass::Prefetch,
            FrameExecutionLane::Worker,
            200U,
            true,
            ticket)) == FrameAdmission::Admitted,
        "elevated prefetch half-cap rejected");
    require(
        scheduler.reserve(request(
            FrameWorkClass::Prefetch,
            FrameExecutionLane::Worker,
            1U,
            true,
            ticket)) == FrameAdmission::ClassBudgetExhausted,
        "elevated prefetch cap overrun admitted");

    scheduler.begin_frame(FramePressure::Critical);
    scheduler.finish_visible_phase();
    require(
        scheduler.reserve(request(
            FrameWorkClass::Background,
            FrameExecutionLane::Worker,
            1U)) == FrameAdmission::SuppressedByPressure,
        "critical background work not suppressed");
    require(
        scheduler.reserve(request(
            FrameWorkClass::Maintenance,
            FrameExecutionLane::Worker,
            1U)) == FrameAdmission::SuppressedByPressure,
        "critical maintenance work not suppressed");
    require(
        scheduler.reserve(request(
            FrameWorkClass::Visible,
            FrameExecutionLane::Ui,
            1U)) == FrameAdmission::VisiblePhaseClosed,
        "visible phase reopened after explicit completion");
}

void test_hidden_page_suppression_and_prefetch_invalidation() {
    FrameBudgetScheduler scheduler(policy());
    const PrefetchTicket moving = scheduler.update_scroll_motion(4096);
    require(moving.direction == 1, "moving prefetch direction not established");

    scheduler.begin_frame(FramePressure::Normal, FrameVisibility::Hidden);
    const PrefetchTicket hidden_ticket = scheduler.current_prefetch_ticket();
    require(hidden_ticket.direction == 0, "hidden frame did not neutralize scroll direction");
    require(hidden_ticket.epoch > moving.epoch, "hidden frame did not invalidate prefetch epoch");

    require(
        scheduler.reserve(request(
            FrameWorkClass::Visible,
            FrameExecutionLane::Ui,
            1U)) == FrameAdmission::SuppressedByVisibility,
        "hidden page admitted visible frame work");
    scheduler.finish_visible_phase();
    require(
        scheduler.reserve(request(
            FrameWorkClass::Background,
            FrameExecutionLane::Worker,
            1U)) == FrameAdmission::SuppressedByVisibility,
        "hidden page admitted background work");
    require(
        scheduler.reserve(request(
            FrameWorkClass::Maintenance,
            FrameExecutionLane::Worker,
            1U)) == FrameAdmission::SuppressedByVisibility,
        "hidden page admitted maintenance work");
    require(
        scheduler.reserve(request(
            FrameWorkClass::Prefetch,
            FrameExecutionLane::Worker,
            1U,
            true,
            moving)) == FrameAdmission::SuppressedByVisibility,
        "hidden page admitted stale prefetch work");

    const FrameBudgetSnapshot hidden = scheduler.snapshot();
    require(hidden.visibility == FrameVisibility::Hidden, "hidden visibility missing from snapshot");
    require(hidden.hidden_frames == 1U, "hidden frame counter mismatch");
    require(hidden.hidden_rejections == 4U, "hidden rejection counter mismatch");
    require(hidden.admitted_requests == 0U, "hidden frame admitted work");
    require(hidden.rejected_requests == 4U, "hidden frame rejection count mismatch");
    require(hidden.spent_us == 0U, "hidden frame consumed CPU budget");
    require(hidden.remaining_us == 0U, "hidden frame exposed spendable remaining budget");

    scheduler.begin_frame(FramePressure::Normal, FrameVisibility::Visible);
    const PrefetchTicket resumed = scheduler.update_scroll_motion(4096);
    require(resumed.direction == 1, "visible resume did not restore scroll direction");
    require(resumed.epoch > hidden_ticket.epoch, "visible resume did not issue fresh prefetch epoch");
    require(
        scheduler.reserve(request(
            FrameWorkClass::Visible,
            FrameExecutionLane::Ui,
            100U)) == FrameAdmission::Admitted,
        "visible resume could not schedule visible work");
    const FrameBudgetSnapshot visible = scheduler.snapshot();
    require(visible.visibility == FrameVisibility::Visible, "visible resume snapshot incorrect");
    require(visible.hidden_frames == 1U, "hidden frame history lost after resume");
    require(visible.spent_us == 100U, "visible resume spend mismatch");
}

void test_frame_reset_and_large_budget_accounting() {
    FrameBudgetScheduler scheduler(policy());
    scheduler.begin_frame();
    require(
        scheduler.reserve(request(
            FrameWorkClass::Visible,
            FrameExecutionLane::Ui,
            900U)) == FrameAdmission::Admitted,
        "first-frame visible reservation rejected");
    const std::uint64_t first_sequence = scheduler.snapshot().frame_sequence;

    scheduler.begin_frame();
    const FrameBudgetSnapshot reset = scheduler.snapshot();
    require(reset.frame_sequence > first_sequence, "frame sequence did not advance");
    require(reset.spent_us == 0U, "frame spend did not reset");
    require(reset.remaining_us == 1000U, "frame budget did not reset");
    require(!reset.visible_phase_complete, "visible phase did not reset");

    FrameBudgetScheduler large(FrameBudgetPolicy{
        std::numeric_limits<std::uint32_t>::max(),
        0U,
        0U,
        0U});
    large.begin_frame();
    require(
        large.reserve(request(
            FrameWorkClass::Visible,
            FrameExecutionLane::Ui,
            std::numeric_limits<std::uint32_t>::max())) == FrameAdmission::Admitted,
        "maximum u32 frame reservation rejected");
    require(large.snapshot().remaining_us == 0U, "maximum frame remaining wrapped");
    require(
        large.reserve(request(
            FrameWorkClass::Visible,
            FrameExecutionLane::Ui,
            1U)) == FrameAdmission::FrameBudgetExhausted,
        "exhausted maximum frame admitted extra work");
}

void test_invalid_request() {
    FrameBudgetScheduler scheduler(policy());
    scheduler.begin_frame();
    require(
        scheduler.reserve(request(
            FrameWorkClass::Visible,
            FrameExecutionLane::Ui,
            0U)) == FrameAdmission::InvalidRequest,
        "zero reservation admitted");

    FrameBudgetScheduler invalid(FrameBudgetPolicy{});
    invalid.begin_frame();
    require(
        invalid.reserve(request(
            FrameWorkClass::Visible,
            FrameExecutionLane::Ui,
            1U)) == FrameAdmission::InvalidRequest,
        "invalid policy admitted work");
}

} // namespace

int main() {
    test_policy_validation();
    test_visible_first_and_leftover_budget();
    test_optional_class_caps();
    test_blocking_ui_rejected();
    test_prefetch_epoch_cancellation();
    test_pressure_suppression();
    test_hidden_page_suppression_and_prefetch_invalidation();
    test_frame_reset_and_large_budget_accounting();
    test_invalid_request();
    std::cout << "Zevryon frame-budget scheduler core tests passed\n";
    return 0;
}
