#include "frame_budget_scheduler.hpp"

#include <algorithm>
#include <limits>

namespace zevryon::massivedoc {
namespace {

std::uint64_t saturating_increment(std::uint64_t value) noexcept {
    if (value == std::numeric_limits<std::uint64_t>::max()) {
        return value;
    }
    return value + 1U;
}

std::uint32_t narrow_u32(std::uint64_t value) noexcept {
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(value, std::numeric_limits<std::uint32_t>::max()));
}

std::uint32_t pressure_cap(std::uint32_t cap, FramePressure pressure) noexcept {
    switch (pressure) {
    case FramePressure::Normal:
        return cap;
    case FramePressure::Elevated:
        return cap / 2U;
    case FramePressure::Critical:
        return 0U;
    }
    return 0U;
}

} // namespace

bool FrameBudgetPolicy::valid() const noexcept {
    return frame_budget_us != 0U && prefetch_budget_us <= frame_budget_us &&
           background_budget_us <= frame_budget_us &&
           maintenance_budget_us <= frame_budget_us;
}

FrameBudgetScheduler::FrameBudgetScheduler(FrameBudgetPolicy policy) noexcept
    : policy_(policy) {}

bool FrameBudgetScheduler::valid() const noexcept {
    return policy_.valid();
}

void FrameBudgetScheduler::begin_frame(FramePressure pressure) noexcept {
    frame_sequence_ = saturating_increment(frame_sequence_);
    admitted_requests_ = 0U;
    rejected_requests_ = 0U;
    spent_us_ = 0U;
    visible_spent_us_ = 0U;
    prefetch_spent_us_ = 0U;
    background_spent_us_ = 0U;
    maintenance_spent_us_ = 0U;
    pressure_ = pressure;
    visible_phase_complete_ = false;
}

void FrameBudgetScheduler::finish_visible_phase() noexcept {
    visible_phase_complete_ = true;
}

PrefetchTicket FrameBudgetScheduler::update_scroll_motion(
    std::int64_t velocity_q8_per_second) noexcept {
    const std::int8_t direction = velocity_q8_per_second < 0
                                      ? static_cast<std::int8_t>(-1)
                                      : velocity_q8_per_second > 0
                                            ? static_cast<std::int8_t>(1)
                                            : static_cast<std::int8_t>(0);
    if (direction != scroll_direction_) {
        scroll_direction_ = direction;
        prefetch_epoch_ = saturating_increment(prefetch_epoch_);
    }
    return current_prefetch_ticket();
}

PrefetchTicket FrameBudgetScheduler::current_prefetch_ticket() const noexcept {
    return PrefetchTicket{prefetch_epoch_, scroll_direction_};
}

std::uint32_t FrameBudgetScheduler::optional_cap(FrameWorkClass work_class) const noexcept {
    switch (work_class) {
    case FrameWorkClass::Visible:
        return policy_.frame_budget_us;
    case FrameWorkClass::Prefetch:
        return pressure_cap(policy_.prefetch_budget_us, pressure_);
    case FrameWorkClass::Background:
        return pressure_cap(policy_.background_budget_us, pressure_);
    case FrameWorkClass::Maintenance:
        return pressure_cap(policy_.maintenance_budget_us, pressure_);
    }
    return 0U;
}

std::uint32_t FrameBudgetScheduler::class_spent(FrameWorkClass work_class) const noexcept {
    switch (work_class) {
    case FrameWorkClass::Visible:
        return narrow_u32(visible_spent_us_);
    case FrameWorkClass::Prefetch:
        return narrow_u32(prefetch_spent_us_);
    case FrameWorkClass::Background:
        return narrow_u32(background_spent_us_);
    case FrameWorkClass::Maintenance:
        return narrow_u32(maintenance_spent_us_);
    }
    return 0U;
}

void FrameBudgetScheduler::add_class_spent(
    FrameWorkClass work_class,
    std::uint32_t reserve_us) noexcept {
    switch (work_class) {
    case FrameWorkClass::Visible:
        visible_spent_us_ += reserve_us;
        break;
    case FrameWorkClass::Prefetch:
        prefetch_spent_us_ += reserve_us;
        break;
    case FrameWorkClass::Background:
        background_spent_us_ += reserve_us;
        break;
    case FrameWorkClass::Maintenance:
        maintenance_spent_us_ += reserve_us;
        break;
    }
}

void FrameBudgetScheduler::record_rejection() noexcept {
    rejected_requests_ = saturating_increment(rejected_requests_);
}

FrameAdmission FrameBudgetScheduler::reserve(const FrameWorkRequest& request) noexcept {
    if (!valid() || request.reserve_us == 0U) {
        record_rejection();
        return FrameAdmission::InvalidRequest;
    }
    if (request.may_block && request.lane == FrameExecutionLane::Ui) {
        record_rejection();
        return FrameAdmission::BlockingOnUi;
    }

    if (request.work_class == FrameWorkClass::Visible) {
        if (visible_phase_complete_) {
            record_rejection();
            return FrameAdmission::VisiblePhaseClosed;
        }
    } else if (!visible_phase_complete_) {
        record_rejection();
        return FrameAdmission::VisiblePhaseOpen;
    }

    if (request.work_class == FrameWorkClass::Prefetch) {
        const PrefetchTicket current = current_prefetch_ticket();
        if (current.direction == 0 || request.prefetch_ticket != current) {
            record_rejection();
            return FrameAdmission::StalePrefetch;
        }
    }

    const std::uint32_t cap = optional_cap(request.work_class);
    if (request.work_class != FrameWorkClass::Visible) {
        if (cap == 0U && pressure_ != FramePressure::Normal) {
            record_rejection();
            return FrameAdmission::SuppressedByPressure;
        }
        const std::uint32_t spent = class_spent(request.work_class);
        if (spent > cap || request.reserve_us > cap - spent) {
            record_rejection();
            return FrameAdmission::ClassBudgetExhausted;
        }
    }

    if (spent_us_ > policy_.frame_budget_us ||
        request.reserve_us > policy_.frame_budget_us - spent_us_) {
        record_rejection();
        return FrameAdmission::FrameBudgetExhausted;
    }

    spent_us_ += request.reserve_us;
    add_class_spent(request.work_class, request.reserve_us);
    admitted_requests_ = saturating_increment(admitted_requests_);
    return FrameAdmission::Admitted;
}

FrameBudgetSnapshot FrameBudgetScheduler::snapshot() const noexcept {
    const std::uint64_t remaining = spent_us_ >= policy_.frame_budget_us
                                        ? 0U
                                        : static_cast<std::uint64_t>(policy_.frame_budget_us) - spent_us_;
    FrameBudgetSnapshot result;
    result.frame_sequence = frame_sequence_;
    result.prefetch_epoch = prefetch_epoch_;
    result.admitted_requests = admitted_requests_;
    result.rejected_requests = rejected_requests_;
    result.spent_us = narrow_u32(spent_us_);
    result.visible_spent_us = narrow_u32(visible_spent_us_);
    result.prefetch_spent_us = narrow_u32(prefetch_spent_us_);
    result.background_spent_us = narrow_u32(background_spent_us_);
    result.maintenance_spent_us = narrow_u32(maintenance_spent_us_);
    result.remaining_us = narrow_u32(remaining);
    result.scroll_direction = scroll_direction_;
    result.pressure = pressure_;
    result.visible_phase_complete = visible_phase_complete_;
    return result;
}

const FrameBudgetPolicy& FrameBudgetScheduler::policy() const noexcept {
    return policy_;
}

} // namespace zevryon::massivedoc
