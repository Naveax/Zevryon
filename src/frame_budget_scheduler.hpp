#pragma once

#include <cstdint>

namespace zevryon::massivedoc {

enum class FrameWorkClass : std::uint8_t {
    Visible = 0,
    Prefetch,
    Background,
    Maintenance,
};

enum class FrameExecutionLane : std::uint8_t {
    Ui = 0,
    Worker,
};

enum class FramePressure : std::uint8_t {
    Normal = 0,
    Elevated,
    Critical,
};

enum class FrameVisibility : std::uint8_t {
    Visible = 0,
    Hidden,
};

enum class FrameAdmission : std::uint8_t {
    Admitted = 0,
    InvalidRequest,
    SuppressedByVisibility,
    VisiblePhaseOpen,
    VisiblePhaseClosed,
    BlockingOnUi,
    StalePrefetch,
    SuppressedByPressure,
    ClassBudgetExhausted,
    FrameBudgetExhausted,
};

struct FrameBudgetPolicy {
    std::uint32_t frame_budget_us{0};
    std::uint32_t prefetch_budget_us{0};
    std::uint32_t background_budget_us{0};
    std::uint32_t maintenance_budget_us{0};

    bool valid() const noexcept;
};

struct PrefetchTicket {
    std::uint64_t epoch{0};
    std::int8_t direction{0};

    bool operator==(const PrefetchTicket&) const noexcept = default;
};

struct FrameWorkRequest {
    FrameWorkClass work_class{FrameWorkClass::Visible};
    FrameExecutionLane lane{FrameExecutionLane::Ui};
    std::uint32_t reserve_us{0};
    bool may_block{false};
    PrefetchTicket prefetch_ticket{};
};

struct FrameBudgetSnapshot {
    std::uint64_t frame_sequence{0};
    std::uint64_t prefetch_epoch{0};
    std::uint64_t admitted_requests{0};
    std::uint64_t rejected_requests{0};
    std::uint64_t hidden_frames{0};
    std::uint64_t hidden_rejections{0};
    std::uint32_t spent_us{0};
    std::uint32_t visible_spent_us{0};
    std::uint32_t prefetch_spent_us{0};
    std::uint32_t background_spent_us{0};
    std::uint32_t maintenance_spent_us{0};
    std::uint32_t remaining_us{0};
    std::int8_t scroll_direction{0};
    FramePressure pressure{FramePressure::Normal};
    FrameVisibility visibility{FrameVisibility::Visible};
    bool visible_phase_complete{false};
};

class FrameBudgetScheduler {
public:
    explicit FrameBudgetScheduler(FrameBudgetPolicy policy) noexcept;

    bool valid() const noexcept;
    void begin_frame(
        FramePressure pressure = FramePressure::Normal,
        FrameVisibility visibility = FrameVisibility::Visible) noexcept;
    void finish_visible_phase() noexcept;

    PrefetchTicket update_scroll_motion(std::int64_t velocity_q8_per_second) noexcept;
    PrefetchTicket current_prefetch_ticket() const noexcept;

    FrameAdmission reserve(const FrameWorkRequest& request) noexcept;
    FrameBudgetSnapshot snapshot() const noexcept;
    const FrameBudgetPolicy& policy() const noexcept;

private:
    std::uint32_t optional_cap(FrameWorkClass work_class) const noexcept;
    std::uint32_t class_spent(FrameWorkClass work_class) const noexcept;
    void add_class_spent(FrameWorkClass work_class, std::uint32_t reserve_us) noexcept;
    void record_rejection() noexcept;
    void invalidate_prefetch_motion() noexcept;

    FrameBudgetPolicy policy_{};
    std::uint64_t frame_sequence_{0};
    std::uint64_t prefetch_epoch_{1};
    std::uint64_t admitted_requests_{0};
    std::uint64_t rejected_requests_{0};
    std::uint64_t hidden_frames_{0};
    std::uint64_t hidden_rejections_{0};
    std::uint64_t spent_us_{0};
    std::uint64_t visible_spent_us_{0};
    std::uint64_t prefetch_spent_us_{0};
    std::uint64_t background_spent_us_{0};
    std::uint64_t maintenance_spent_us_{0};
    std::int8_t scroll_direction_{0};
    FramePressure pressure_{FramePressure::Normal};
    FrameVisibility visibility_{FrameVisibility::Visible};
    bool visible_phase_complete_{false};
};

} // namespace zevryon::massivedoc
