#include "zenith_tab_runtime.hpp"

#include "massivedoc_store.hpp"
#include "shared_source_prefetch_pool.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace zevryon::massivedoc {
namespace {

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
    return left > std::numeric_limits<std::uint64_t>::max() - right
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}

std::uint32_t elapsed_budget_charge(
    std::uint64_t elapsed_us,
    std::uint32_t frame_budget_us) noexcept {
    if (frame_budget_us == 0U) {
        return 0U;
    }
    const std::uint64_t at_least_one = std::max<std::uint64_t>(1U, elapsed_us);
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(at_least_one, frame_budget_us));
}

bool schedule_result_counts_as_accept(SharedSourcePrefetchScheduleResult result) noexcept {
    return result == SharedSourcePrefetchScheduleResult::accepted;
}

bool schedule_result_counts_as_coalesce(SharedSourcePrefetchScheduleResult result) noexcept {
    return result == SharedSourcePrefetchScheduleResult::coalesced;
}

bool schedule_result_counts_as_replace(SharedSourcePrefetchScheduleResult result) noexcept {
    return result == SharedSourcePrefetchScheduleResult::replaced;
}

} // namespace

bool ZenithTabRuntimeConfig::valid() const noexcept {
    return frame_budget.valid() && prefetch_reserve_us > 0U &&
           prefetch_reserve_us <= frame_budget.prefetch_budget_us &&
           prefetch_bytes > 0U && prefetch_bytes <= kIoWindowBytes &&
           layout.average_advance_q8 > 0U && layout.line_height_q8 > 0U &&
           layout.width_bucket_q8 > 0U && layout.checkpoint_stride_bytes > 0U;
}

struct ZenithTabRuntime::Impl {
    Impl(
        const std::filesystem::path& root_value,
        SharedSourcePrefetchPool* pool_value,
        std::uint64_t session_id_value,
        ZenithTabRuntimeConfig config_value)
        : root(root_value),
          pool(pool_value),
          session_id(session_id_value),
          config(config_value),
          hot_scroll(root_value, config_value.layout),
          scheduler(config_value.frame_budget) {}

    std::filesystem::path root;
    SharedSourcePrefetchPool* pool{nullptr};
    std::uint64_t session_id{0U};
    ZenithTabRuntimeConfig config;
    ZenithHotScrollSession hot_scroll;
    FrameBudgetScheduler scheduler;
    FrameVisibility visibility{FrameVisibility::Visible};
    FramePressure pressure{FramePressure::Normal};
    std::int64_t scroll_velocity_q8_per_second{0};
    ZenithTabRuntimeStats statistics;
    bool opened{false};
    bool pool_registered{false};

    void drain_ready_prefetch() noexcept {
        if (!pool_registered || pool == nullptr) {
            return;
        }
        SourceWindowPrefetchResult ready;
        if (!pool->try_take_ready(session_id, &ready)) {
            return;
        }
        statistics.prefetch_ready_drains =
            saturating_add(statistics.prefetch_ready_drains, 1U);
        statistics.prefetch_ready_bytes_drained = saturating_add(
            statistics.prefetch_ready_bytes_drained,
            static_cast<std::uint64_t>(ready.bytes.size()));
        if (ready.succeeded) {
            statistics.prefetch_success_drains =
                saturating_add(statistics.prefetch_success_drains, 1U);
        } else {
            statistics.prefetch_failure_drains =
                saturating_add(statistics.prefetch_failure_drains, 1U);
        }
    }

    bool choose_prefetch_request(
        const LayoutWindowResult& result,
        SourceWindowPrefetchRequest* request) const noexcept {
        if (request == nullptr || result.fragments.empty()) {
            return false;
        }
        const PrefetchTicket ticket = scheduler.current_prefetch_ticket();
        if (ticket.direction == 0) {
            return false;
        }

        const LayoutFragment& fragment = ticket.direction > 0
                                             ? result.fragments.back()
                                             : result.fragments.front();
        request->record_index = fragment.source_record_index;
        request->max_bytes = config.prefetch_bytes;
        request->ticket = ticket;
        if (ticket.direction > 0) {
            request->byte_offset = fragment.source_end;
            return true;
        }
        if (fragment.source_start == 0U) {
            return false;
        }
        request->byte_offset =
            fragment.source_start > config.prefetch_bytes
                ? fragment.source_start - config.prefetch_bytes
                : 0U;
        return request->byte_offset < fragment.source_start;
    }

    void schedule_prefetch(const LayoutWindowResult& result) noexcept {
        if (!pool_registered || pool == nullptr ||
            visibility != FrameVisibility::Visible ||
            pressure == FramePressure::Critical) {
            return;
        }

        SourceWindowPrefetchRequest request;
        if (!choose_prefetch_request(result, &request)) {
            return;
        }

        FrameWorkRequest budget_request;
        budget_request.work_class = FrameWorkClass::Prefetch;
        budget_request.lane = FrameExecutionLane::Worker;
        budget_request.reserve_us = config.prefetch_reserve_us;
        budget_request.may_block = true;
        budget_request.prefetch_ticket = request.ticket;
        if (scheduler.reserve(budget_request) != FrameAdmission::Admitted) {
            statistics.prefetch_schedule_rejections =
                saturating_add(statistics.prefetch_schedule_rejections, 1U);
            return;
        }
        statistics.prefetch_admissions =
            saturating_add(statistics.prefetch_admissions, 1U);

        const SharedSourcePrefetchScheduleResult scheduled =
            pool->request(session_id, request);
        if (schedule_result_counts_as_accept(scheduled)) {
            statistics.prefetch_schedule_accepts =
                saturating_add(statistics.prefetch_schedule_accepts, 1U);
        } else if (schedule_result_counts_as_coalesce(scheduled)) {
            statistics.prefetch_schedule_coalesces =
                saturating_add(statistics.prefetch_schedule_coalesces, 1U);
        } else if (schedule_result_counts_as_replace(scheduled)) {
            statistics.prefetch_schedule_replacements =
                saturating_add(statistics.prefetch_schedule_replacements, 1U);
        } else {
            statistics.prefetch_schedule_rejections =
                saturating_add(statistics.prefetch_schedule_rejections, 1U);
        }
    }
};

ZenithTabRuntime::ZenithTabRuntime(
    const std::filesystem::path& store_root,
    SharedSourcePrefetchPool* shared_prefetch_pool,
    std::uint64_t session_id,
    ZenithTabRuntimeConfig config)
    : impl_(std::make_unique<Impl>(
          store_root,
          shared_prefetch_pool,
          session_id,
          config)) {}

ZenithTabRuntime::~ZenithTabRuntime() {
    if (impl_->pool_registered && impl_->pool != nullptr) {
        impl_->pool->close_session(impl_->session_id);
    }
}

bool ZenithTabRuntime::open(std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (impl_->opened) {
        *error = "zenith tab runtime is already open";
        return false;
    }
    if (!impl_->config.valid() || !impl_->scheduler.valid()) {
        *error = "invalid zenith tab runtime configuration";
        return false;
    }
    if (!impl_->hot_scroll.open(error)) {
        return false;
    }
    if (impl_->pool != nullptr) {
        if (!impl_->pool->open_session(
                impl_->session_id,
                impl_->root,
                impl_->scheduler.current_prefetch_ticket(),
                true)) {
            *error = "unable to register zenith tab with shared prefetch pool";
            return false;
        }
        impl_->pool_registered = true;
    }
    impl_->opened = true;
    return true;
}

bool ZenithTabRuntime::set_activity(
    FrameVisibility visibility,
    FramePressure pressure,
    std::int64_t scroll_velocity_q8_per_second,
    std::string* error) {
    if (!impl_->opened || error == nullptr) {
        if (error != nullptr) {
            *error = "invalid zenith tab activity update";
        }
        return false;
    }
    error->clear();
    impl_->visibility = visibility;
    impl_->pressure = pressure;
    impl_->scroll_velocity_q8_per_second = scroll_velocity_q8_per_second;

    PrefetchTicket authority;
    bool active = visibility == FrameVisibility::Visible;
    if (!active) {
        authority = impl_->scheduler.update_scroll_motion(0);
        if (pressure == FramePressure::Critical) {
            impl_->hot_scroll.trim_memory(ZenithMemoryPressure::Critical);
            impl_->statistics.critical_transitions =
                saturating_add(impl_->statistics.critical_transitions, 1U);
        } else {
            impl_->hot_scroll.trim_memory(ZenithMemoryPressure::Background);
            impl_->statistics.background_transitions =
                saturating_add(impl_->statistics.background_transitions, 1U);
        }
    } else {
        authority = impl_->scheduler.update_scroll_motion(
            scroll_velocity_q8_per_second);
    }

    if (impl_->pool_registered && impl_->pool != nullptr &&
        !impl_->pool->set_session_authority(
            impl_->session_id,
            authority,
            active)) {
        *error = "unable to update zenith tab prefetch authority";
        return false;
    }
    return true;
}

bool ZenithTabRuntime::layout(
    std::uint64_t scroll_y_q8,
    std::uint32_t viewport_width_q8,
    std::uint64_t viewport_height_q8,
    std::uint64_t overscan_q8,
    std::size_t max_fragments,
    LayoutWindowResult* result,
    bool* used_checkpoint_path,
    std::string* error) {
    if (!impl_->opened || result == nullptr || used_checkpoint_path == nullptr ||
        error == nullptr) {
        if (error != nullptr) {
            *error = "invalid zenith tab layout request";
        }
        return false;
    }

    impl_->statistics.layout_requests =
        saturating_add(impl_->statistics.layout_requests, 1U);
    impl_->scheduler.begin_frame(impl_->pressure, impl_->visibility);

    if (impl_->visibility == FrameVisibility::Hidden) {
        *result = LayoutWindowResult{};
        *used_checkpoint_path = false;
        error->clear();
        impl_->statistics.hidden_layout_suppressions = saturating_add(
            impl_->statistics.hidden_layout_suppressions, 1U);
        return true;
    }

    impl_->drain_ready_prefetch();
    const auto started = std::chrono::steady_clock::now();
    const bool success = impl_->hot_scroll.layout(
        scroll_y_q8,
        viewport_width_q8,
        viewport_height_q8,
        overscan_q8,
        max_fragments,
        result,
        used_checkpoint_path,
        error);
    const auto finished = std::chrono::steady_clock::now();
    const std::uint64_t elapsed_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count());
    impl_->statistics.last_visible_layout_us = elapsed_us;
    impl_->statistics.peak_visible_layout_us = std::max(
        impl_->statistics.peak_visible_layout_us,
        elapsed_us);
    impl_->statistics.visible_layouts =
        saturating_add(impl_->statistics.visible_layouts, 1U);
    if (elapsed_us > impl_->config.frame_budget.frame_budget_us) {
        impl_->statistics.visible_frame_overruns = saturating_add(
            impl_->statistics.visible_frame_overruns, 1U);
    }

    FrameWorkRequest visible_charge;
    visible_charge.work_class = FrameWorkClass::Visible;
    visible_charge.lane = FrameExecutionLane::Ui;
    visible_charge.reserve_us = elapsed_budget_charge(
        elapsed_us,
        impl_->config.frame_budget.frame_budget_us);
    visible_charge.may_block = false;
    static_cast<void>(impl_->scheduler.reserve(visible_charge));
    impl_->scheduler.finish_visible_phase();

    if (!success) {
        return false;
    }
    impl_->schedule_prefetch(*result);
    return true;
}

FrameVisibility ZenithTabRuntime::visibility() const noexcept {
    return impl_->visibility;
}

FramePressure ZenithTabRuntime::pressure() const noexcept {
    return impl_->pressure;
}

PrefetchTicket ZenithTabRuntime::prefetch_ticket() const noexcept {
    return impl_->scheduler.current_prefetch_ticket();
}

const ZenithTabRuntimeStats& ZenithTabRuntime::stats() const noexcept {
    return impl_->statistics;
}

const ZenithHotScrollStats& ZenithTabRuntime::hot_scroll_stats() const noexcept {
    return impl_->hot_scroll.stats();
}

FrameBudgetSnapshot ZenithTabRuntime::frame_budget_snapshot() const noexcept {
    return impl_->scheduler.snapshot();
}

} // namespace zevryon::massivedoc
