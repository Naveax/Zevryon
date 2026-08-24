#pragma once

#include "frame_budget_scheduler.hpp"
#include "layout_window.hpp"
#include "zenith_hot_scroll.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace zevryon::massivedoc {

class SharedRecordLengthAuthority;
class SharedSourcePrefetchPool;

struct ZenithTabRuntimeConfig {
    LayoutConfig layout{};
    FrameBudgetPolicy frame_budget{16'667U, 2'000U, 500U, 250U};
    std::uint32_t prefetch_reserve_us{200U};
    std::size_t prefetch_bytes{64U * 1024U};

    // Non-owning process-shared metadata authority. The authority, when set,
    // must outlive this runtime. Cache-only lookups never perform disk I/O.
    SharedRecordLengthAuthority* record_length_authority{nullptr};

    bool valid() const noexcept;
};

struct ZenithTabRuntimeStats {
    std::uint64_t layout_requests{0U};
    std::uint64_t visible_layouts{0U};
    std::uint64_t hidden_layout_suppressions{0U};
    std::uint64_t ui_blocking_layout_rejections{0U};
    std::uint64_t visible_frame_overruns{0U};
    std::uint64_t background_transitions{0U};
    std::uint64_t critical_transitions{0U};
    std::uint64_t prefetch_admissions{0U};
    std::uint64_t prefetch_schedule_accepts{0U};
    std::uint64_t prefetch_schedule_coalesces{0U};
    std::uint64_t prefetch_schedule_replacements{0U};
    std::uint64_t prefetch_schedule_rejections{0U};
    std::uint64_t prefetch_ready_drains{0U};
    std::uint64_t prefetch_ready_bytes_drained{0U};
    std::uint64_t prefetch_success_drains{0U};
    std::uint64_t prefetch_failure_drains{0U};
    std::uint64_t prefetch_cache_admissions{0U};
    std::uint64_t prefetch_cache_rejections{0U};
    std::uint64_t record_length_cache_hits{0U};
    std::uint64_t record_length_clamps{0U};
    std::uint64_t record_length_eof_suppressions{0U};
    std::uint64_t record_length_learns{0U};
    std::uint64_t record_length_learn_failures{0U};
    std::uint64_t last_visible_layout_us{0U};
    std::uint64_t peak_visible_layout_us{0U};
};

class ZenithTabRuntime final {
public:
    ZenithTabRuntime(
        const std::filesystem::path& store_root,
        SharedSourcePrefetchPool* shared_prefetch_pool,
        std::uint64_t session_id,
        ZenithTabRuntimeConfig config = {});
    ~ZenithTabRuntime();

    ZenithTabRuntime(const ZenithTabRuntime&) = delete;
    ZenithTabRuntime& operator=(const ZenithTabRuntime&) = delete;
    ZenithTabRuntime(ZenithTabRuntime&&) = delete;
    ZenithTabRuntime& operator=(ZenithTabRuntime&&) = delete;

    bool open(std::string* error);
    bool set_activity(
        FrameVisibility visibility,
        FramePressure pressure,
        std::int64_t scroll_velocity_q8_per_second,
        std::string* error);

    // Synchronous compatibility entry point. The current hot-scroll layout can
    // perform checkpoint/source I/O on cache misses, so this method is worker-
    // lane authority and must not be called from a UI execution lane.
    bool layout(
        std::uint64_t scroll_y_q8,
        std::uint32_t viewport_width_q8,
        std::uint64_t viewport_height_q8,
        std::uint64_t overscan_q8,
        std::size_t max_fragments,
        LayoutWindowResult* result,
        bool* used_checkpoint_path,
        std::string* error);

    // Explicit lane-aware entry point. Visible UI-lane calls fail closed before
    // entering the hot-scroll engine because that path can still block on disk.
    // Hidden calls remain suppressible on either lane because they perform no
    // foreground layout work.
    bool layout_on_lane(
        FrameExecutionLane lane,
        std::uint64_t scroll_y_q8,
        std::uint32_t viewport_width_q8,
        std::uint64_t viewport_height_q8,
        std::uint64_t overscan_q8,
        std::size_t max_fragments,
        LayoutWindowResult* result,
        bool* used_checkpoint_path,
        std::string* error);

    FrameVisibility visibility() const noexcept;
    FramePressure pressure() const noexcept;
    PrefetchTicket prefetch_ticket() const noexcept;
    const ZenithTabRuntimeStats& stats() const noexcept;
    const ZenithHotScrollStats& hot_scroll_stats() const noexcept;
    FrameBudgetSnapshot frame_budget_snapshot() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zevryon::massivedoc
