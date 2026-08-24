#include "zenith_tab_runtime.hpp"

#include "massivedoc_store.hpp"
#include "prefetch_tail_admission.hpp"
#include "runtime_prefetch_record_policy.hpp"
#include "shared_source_prefetch_pool.hpp"
#include "velocity_prefetch_planner.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
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

bool async_schedule_counts_as_accept(
    ForegroundLayoutWorkerScheduleResult result) noexcept {
    return result == ForegroundLayoutWorkerScheduleResult::Accepted;
}

bool async_schedule_counts_as_coalesce(
    ForegroundLayoutWorkerScheduleResult result) noexcept {
    return result == ForegroundLayoutWorkerScheduleResult::Coalesced;
}

bool async_schedule_counts_as_replace(
    ForegroundLayoutWorkerScheduleResult result) noexcept {
    return result == ForegroundLayoutWorkerScheduleResult::Replaced;
}

std::uint8_t trim_level(FramePressure pressure) noexcept {
    return pressure == FramePressure::Critical ? 2U : 1U;
}

ZenithMemoryPressure trim_pressure(std::uint8_t level) noexcept {
    return level >= 2U
               ? ZenithMemoryPressure::Critical
               : ZenithMemoryPressure::Background;
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
        SharedForegroundLayoutWorkerPool* foreground_pool_value,
        std::uint64_t session_id_value,
        ZenithTabRuntimeConfig config_value)
        : root(root_value),
          pool(pool_value),
          foreground_pool(foreground_pool_value),
          session_id(session_id_value),
          config(config_value),
          hot_scroll(root_value, config_value.layout),
          scheduler(config_value.frame_budget) {}

    std::filesystem::path root;
    SharedSourcePrefetchPool* pool{nullptr};
    SharedForegroundLayoutWorkerPool* foreground_pool{nullptr};
    std::uint64_t session_id{0U};
    ZenithTabRuntimeConfig config;
    ZenithHotScrollSession hot_scroll;
    FrameBudgetScheduler scheduler;
    std::atomic<FrameVisibility> visibility{FrameVisibility::Visible};
    std::atomic<FramePressure> pressure{FramePressure::Normal};
    std::atomic<std::int64_t> scroll_velocity_q8_per_second{0};
    std::atomic<std::uint8_t> pending_trim{0U};
    std::atomic<bool> deferred_trim_marked{false};
    mutable std::mutex hot_scroll_mutex;
    mutable std::mutex statistics_mutex;
    ZenithTabRuntimeStats statistics;
    bool opened{false};
    bool pool_registered{false};
    bool foreground_registered{false};

    void update_statistics(const auto& function) noexcept {
        try {
            std::lock_guard<std::mutex> lock(statistics_mutex);
            function(statistics);
        } catch (...) {
            // Statistics are diagnostic only; runtime correctness does not
            // depend on acquiring the diagnostic mutex after initialization.
        }
    }

    void queue_trim(FramePressure requested_pressure) noexcept {
        const std::uint8_t requested = trim_level(requested_pressure);
        std::uint8_t observed = pending_trim.load(std::memory_order_relaxed);
        while (observed < requested &&
               !pending_trim.compare_exchange_weak(
                   observed,
                   requested,
                   std::memory_order_release,
                   std::memory_order_relaxed)) {
        }
    }

    void apply_pending_trim_locked() noexcept {
        const std::uint8_t pending =
            pending_trim.exchange(0U, std::memory_order_acq_rel);
        if (pending == 0U) {
            return;
        }
        hot_scroll.trim_memory(trim_pressure(pending));
        if (deferred_trim_marked.exchange(false, std::memory_order_acq_rel)) {
            update_statistics([](ZenithTabRuntimeStats& stats) {
                stats.deferred_trim_applications = saturating_add(
                    stats.deferred_trim_applications,
                    1U);
            });
        }
    }

    void request_trim_without_waiting(FramePressure requested_pressure) noexcept {
        queue_trim(requested_pressure);
        std::unique_lock<std::mutex> lock(hot_scroll_mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            deferred_trim_marked.store(true, std::memory_order_release);
            update_statistics([](ZenithTabRuntimeStats& stats) {
                stats.deferred_trim_requests = saturating_add(
                    stats.deferred_trim_requests,
                    1U);
            });
            return;
        }
        apply_pending_trim_locked();
    }

    void drain_ready_prefetch_locked() noexcept {
        if (!pool_registered || pool == nullptr) {
            return;
        }
        SourceWindowPrefetchResult ready;
        if (!pool->try_take_ready(session_id, &ready)) {
            return;
        }

        const std::uint64_t ready_bytes =
            static_cast<std::uint64_t>(ready.bytes.size());
        PrefetchRecordLengthLearnResult learned =
            PrefetchRecordLengthLearnResult::NotApplicable;
        PrefetchTailAdmissionResult tail_result = PrefetchTailAdmissionResult::Invalid;
        bool admitted = false;
        if (ready.succeeded) {
            learned = learn_record_length_from_short_prefetch(
                config.record_length_authority,
                root,
                ready.request.record_index,
                ready.request.byte_offset,
                ready.request.max_bytes,
                ready.bytes.size());
            tail_result = canonicalize_prefetch_tail_for_exact_admission(&ready);
            if (tail_result != PrefetchTailAdmissionResult::Invalid) {
                admitted = hot_scroll.admit_prefetched_source_window(
                    ready.request.record_index,
                    ready.request.byte_offset,
                    ready.request.max_bytes,
                    std::move(ready.bytes));
            }
        }

        update_statistics([&](ZenithTabRuntimeStats& stats) {
            stats.prefetch_ready_drains =
                saturating_add(stats.prefetch_ready_drains, 1U);
            stats.prefetch_ready_bytes_drained = saturating_add(
                stats.prefetch_ready_bytes_drained,
                ready_bytes);
            if (!ready.succeeded) {
                stats.prefetch_failure_drains =
                    saturating_add(stats.prefetch_failure_drains, 1U);
                return;
            }
            stats.prefetch_success_drains =
                saturating_add(stats.prefetch_success_drains, 1U);
            if (learned == PrefetchRecordLengthLearnResult::Learned) {
                stats.record_length_learns =
                    saturating_add(stats.record_length_learns, 1U);
            } else if (learned == PrefetchRecordLengthLearnResult::Failed) {
                stats.record_length_learn_failures =
                    saturating_add(stats.record_length_learn_failures, 1U);
            }
            if (tail_result == PrefetchTailAdmissionResult::Invalid || !admitted) {
                stats.prefetch_cache_rejections =
                    saturating_add(stats.prefetch_cache_rejections, 1U);
            } else {
                stats.prefetch_cache_admissions =
                    saturating_add(stats.prefetch_cache_admissions, 1U);
            }
        });
    }

    bool choose_prefetch_request(
        const LayoutWindowResult& result,
        SourceWindowPrefetchRequest* request) noexcept {
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
        std::uint64_t source_offset = 0U;
        VelocityPrefetchDecision decision;
        if (!choose_velocity_prefetch_offset(
                ticket.direction,
                scroll_velocity_q8_per_second.load(std::memory_order_acquire),
                fragment.source_start,
                fragment.source_end,
                config.prefetch_bytes,
                VelocityPrefetchPolicy{},
                &source_offset,
                &decision)) {
            return false;
        }

        const std::uint64_t visible_edge = ticket.direction > 0
                                               ? fragment.source_end
                                               : fragment.source_start;
        const RuntimePrefetchRecordPolicyDecision bounded =
            apply_cached_record_bounds(
                config.record_length_authority,
                root,
                fragment.source_record_index,
                ticket.direction,
                visible_edge,
                source_offset,
                config.prefetch_bytes);
        update_statistics([&](ZenithTabRuntimeStats& stats) {
            if (bounded.metadata_hit) {
                stats.record_length_cache_hits =
                    saturating_add(stats.record_length_cache_hits, 1U);
            }
            if (!bounded.should_issue && bounded.eof_suppressed) {
                stats.record_length_eof_suppressions = saturating_add(
                    stats.record_length_eof_suppressions,
                    1U);
            }
            if (bounded.clamped) {
                stats.record_length_clamps =
                    saturating_add(stats.record_length_clamps, 1U);
            }
        });
        if (!bounded.should_issue) {
            return false;
        }

        request->record_index = fragment.source_record_index;
        request->byte_offset = bounded.byte_offset;
        request->max_bytes = bounded.request_bytes;
        request->ticket = ticket;
        request->visible_edge_offset = visible_edge;
        request->has_visible_edge_offset = true;
        return request->max_bytes != 0U;
    }

    void schedule_prefetch(const LayoutWindowResult& result) noexcept {
        if (!pool_registered || pool == nullptr ||
            visibility.load(std::memory_order_acquire) != FrameVisibility::Visible ||
            pressure.load(std::memory_order_acquire) == FramePressure::Critical) {
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
            update_statistics([](ZenithTabRuntimeStats& stats) {
                stats.prefetch_schedule_rejections = saturating_add(
                    stats.prefetch_schedule_rejections,
                    1U);
            });
            return;
        }
        update_statistics([](ZenithTabRuntimeStats& stats) {
            stats.prefetch_admissions =
                saturating_add(stats.prefetch_admissions, 1U);
        });

        const SharedSourcePrefetchScheduleResult scheduled =
            pool->request(session_id, request);
        update_statistics([&](ZenithTabRuntimeStats& stats) {
            if (schedule_result_counts_as_accept(scheduled)) {
                stats.prefetch_schedule_accepts =
                    saturating_add(stats.prefetch_schedule_accepts, 1U);
            } else if (schedule_result_counts_as_coalesce(scheduled)) {
                stats.prefetch_schedule_coalesces =
                    saturating_add(stats.prefetch_schedule_coalesces, 1U);
            } else if (schedule_result_counts_as_replace(scheduled)) {
                stats.prefetch_schedule_replacements = saturating_add(
                    stats.prefetch_schedule_replacements,
                    1U);
            } else {
                stats.prefetch_schedule_rejections = saturating_add(
                    stats.prefetch_schedule_rejections,
                    1U);
            }
        });
    }

    bool execute_hot_scroll_worker(
        const ForegroundLayoutRequest& request,
        LayoutWindowResult* result,
        bool* used_checkpoint_path,
        std::string* error) noexcept {
        if (result == nullptr || used_checkpoint_path == nullptr || error == nullptr) {
            return false;
        }
        if (visibility.load(std::memory_order_acquire) == FrameVisibility::Hidden) {
            *result = LayoutWindowResult{};
            *used_checkpoint_path = false;
            *error = "foreground layout became hidden before worker execution";
            return false;
        }

        std::unique_lock<std::mutex> lock(hot_scroll_mutex);
        if (visibility.load(std::memory_order_acquire) == FrameVisibility::Hidden) {
            *result = LayoutWindowResult{};
            *used_checkpoint_path = false;
            *error = "foreground layout became hidden before hot-scroll execution";
            apply_pending_trim_locked();
            return false;
        }

        drain_ready_prefetch_locked();
        const auto started = std::chrono::steady_clock::now();
        const bool success = hot_scroll.layout(
            request.scroll_y_q8,
            request.viewport_width_q8,
            request.viewport_height_q8,
            request.overscan_q8,
            request.max_fragments,
            result,
            used_checkpoint_path,
            error);
        const auto finished = std::chrono::steady_clock::now();
        apply_pending_trim_locked();
        lock.unlock();

        const std::uint64_t elapsed_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count());
        update_statistics([&](ZenithTabRuntimeStats& stats) {
            stats.last_visible_layout_us = elapsed_us;
            stats.peak_visible_layout_us = std::max(
                stats.peak_visible_layout_us,
                elapsed_us);
            stats.visible_layouts =
                saturating_add(stats.visible_layouts, 1U);
            if (elapsed_us > config.frame_budget.frame_budget_us) {
                stats.visible_frame_overruns = saturating_add(
                    stats.visible_frame_overruns,
                    1U);
            }
        });
        return success;
    }
};

ZenithTabRuntime::ZenithTabRuntime(
    const std::filesystem::path& store_root,
    SharedSourcePrefetchPool* shared_prefetch_pool,
    std::uint64_t session_id,
    ZenithTabRuntimeConfig config)
    : ZenithTabRuntime(
          store_root,
          shared_prefetch_pool,
          nullptr,
          session_id,
          config) {}

ZenithTabRuntime::ZenithTabRuntime(
    const std::filesystem::path& store_root,
    SharedSourcePrefetchPool* shared_prefetch_pool,
    SharedForegroundLayoutWorkerPool* foreground_layout_pool,
    std::uint64_t session_id,
    ZenithTabRuntimeConfig config)
    : impl_(std::make_unique<Impl>(
          store_root,
          shared_prefetch_pool,
          foreground_layout_pool,
          session_id,
          config)) {}

ZenithTabRuntime::~ZenithTabRuntime() {
    if (impl_->foreground_registered && impl_->foreground_pool != nullptr) {
        impl_->foreground_pool->close_session(impl_->session_id);
    }
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
    {
        std::lock_guard<std::mutex> lock(impl_->hot_scroll_mutex);
        if (!impl_->hot_scroll.open(error)) {
            return false;
        }
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
    if (impl_->foreground_pool != nullptr) {
        Impl* implementation = impl_.get();
        if (!impl_->foreground_pool->open_session(
                impl_->session_id,
                [implementation](
                    const ForegroundLayoutRequest& request,
                    LayoutWindowResult* result,
                    bool* used_checkpoint_path,
                    std::string* worker_error) {
                    return implementation->execute_hot_scroll_worker(
                        request,
                        result,
                        used_checkpoint_path,
                        worker_error);
                },
                true)) {
            *error = "unable to register zenith tab with foreground layout pool";
            return false;
        }
        impl_->foreground_registered = true;
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
    impl_->visibility.store(visibility, std::memory_order_release);
    impl_->pressure.store(pressure, std::memory_order_release);
    impl_->scroll_velocity_q8_per_second.store(
        scroll_velocity_q8_per_second,
        std::memory_order_release);

    const bool active = visibility == FrameVisibility::Visible;
    const PrefetchTicket authority = impl_->scheduler.update_scroll_motion(
        active ? scroll_velocity_q8_per_second : 0);

    if (impl_->foreground_registered && impl_->foreground_pool != nullptr &&
        !impl_->foreground_pool->set_session_active(impl_->session_id, active)) {
        *error = "unable to update zenith tab foreground layout authority";
        return false;
    }
    if (impl_->pool_registered && impl_->pool != nullptr &&
        !impl_->pool->set_session_authority(
            impl_->session_id,
            authority,
            active)) {
        *error = "unable to update zenith tab prefetch authority";
        return false;
    }

    if (!active) {
        impl_->update_statistics([&](ZenithTabRuntimeStats& stats) {
            if (pressure == FramePressure::Critical) {
                stats.critical_transitions =
                    saturating_add(stats.critical_transitions, 1U);
            } else {
                stats.background_transitions =
                    saturating_add(stats.background_transitions, 1U);
            }
        });
        impl_->request_trim_without_waiting(pressure);
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
    return layout_on_lane(
        FrameExecutionLane::Worker,
        scroll_y_q8,
        viewport_width_q8,
        viewport_height_q8,
        overscan_q8,
        max_fragments,
        result,
        used_checkpoint_path,
        error);
}

bool ZenithTabRuntime::layout_on_lane(
    FrameExecutionLane lane,
    std::uint64_t scroll_y_q8,
    std::uint32_t viewport_width_q8,
    std::uint64_t viewport_height_q8,
    std::uint64_t overscan_q8,
    std::size_t max_fragments,
    LayoutWindowResult* result,
    bool* used_checkpoint_path,
    std::string* error) {
    if (!impl_->opened || result == nullptr || used_checkpoint_path == nullptr ||
        error == nullptr ||
        (lane != FrameExecutionLane::Ui && lane != FrameExecutionLane::Worker)) {
        if (error != nullptr) {
            *error = "invalid zenith tab layout request";
        }
        return false;
    }

    impl_->update_statistics([](ZenithTabRuntimeStats& stats) {
        stats.layout_requests = saturating_add(stats.layout_requests, 1U);
    });
    const FrameVisibility visibility =
        impl_->visibility.load(std::memory_order_acquire);
    const FramePressure pressure =
        impl_->pressure.load(std::memory_order_acquire);
    impl_->scheduler.begin_frame(pressure, visibility);

    if (visibility == FrameVisibility::Hidden) {
        *result = LayoutWindowResult{};
        *used_checkpoint_path = false;
        error->clear();
        impl_->update_statistics([](ZenithTabRuntimeStats& stats) {
            stats.hidden_layout_suppressions = saturating_add(
                stats.hidden_layout_suppressions,
                1U);
        });
        return true;
    }

    if (lane == FrameExecutionLane::Ui) {
        *result = LayoutWindowResult{};
        *used_checkpoint_path = false;
        FrameWorkRequest blocked_request;
        blocked_request.work_class = FrameWorkClass::Visible;
        blocked_request.lane = FrameExecutionLane::Ui;
        blocked_request.reserve_us = 1U;
        blocked_request.may_block = true;
        const FrameAdmission admission = impl_->scheduler.reserve(blocked_request);
        if (admission != FrameAdmission::BlockingOnUi) {
            *error = "frame scheduler failed to reject blocking UI layout";
            return false;
        }
        impl_->update_statistics([](ZenithTabRuntimeStats& stats) {
            stats.ui_blocking_layout_rejections = saturating_add(
                stats.ui_blocking_layout_rejections,
                1U);
        });
        *error = "blocking hot-scroll layout is forbidden on the UI execution lane";
        return false;
    }

    if (impl_->foreground_registered) {
        *result = LayoutWindowResult{};
        *used_checkpoint_path = false;
        *error = "synchronous layout is disabled while async foreground layout is registered";
        return false;
    }

    ForegroundLayoutRequest request;
    request.request_id = 1U;
    request.scroll_y_q8 = scroll_y_q8;
    request.viewport_width_q8 = viewport_width_q8;
    request.viewport_height_q8 = viewport_height_q8;
    request.overscan_q8 = overscan_q8;
    request.max_fragments = max_fragments;
    const auto started = std::chrono::steady_clock::now();
    const bool success = impl_->execute_hot_scroll_worker(
        request,
        result,
        used_checkpoint_path,
        error);
    const auto finished = std::chrono::steady_clock::now();
    const std::uint64_t elapsed_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count());

    FrameWorkRequest visible_charge;
    visible_charge.work_class = FrameWorkClass::Visible;
    visible_charge.lane = FrameExecutionLane::Worker;
    visible_charge.reserve_us = elapsed_budget_charge(
        elapsed_us,
        impl_->config.frame_budget.frame_budget_us);
    visible_charge.may_block = true;
    static_cast<void>(impl_->scheduler.reserve(visible_charge));
    impl_->scheduler.finish_visible_phase();

    if (!success) {
        return false;
    }
    impl_->schedule_prefetch(*result);
    return true;
}

ForegroundLayoutWorkerScheduleResult ZenithTabRuntime::request_layout_async(
    ForegroundLayoutRequest request) noexcept {
    if (!impl_->opened || !impl_->foreground_registered ||
        impl_->foreground_pool == nullptr) {
        return ForegroundLayoutWorkerScheduleResult::Invalid;
    }
    impl_->update_statistics([](ZenithTabRuntimeStats& stats) {
        stats.async_layout_requests =
            saturating_add(stats.async_layout_requests, 1U);
    });
    const ForegroundLayoutWorkerScheduleResult scheduled =
        impl_->foreground_pool->request(impl_->session_id, std::move(request));
    impl_->update_statistics([&](ZenithTabRuntimeStats& stats) {
        if (async_schedule_counts_as_accept(scheduled)) {
            stats.async_layout_accepts =
                saturating_add(stats.async_layout_accepts, 1U);
        } else if (async_schedule_counts_as_coalesce(scheduled)) {
            stats.async_layout_coalesces =
                saturating_add(stats.async_layout_coalesces, 1U);
        } else if (async_schedule_counts_as_replace(scheduled)) {
            stats.async_layout_replacements =
                saturating_add(stats.async_layout_replacements, 1U);
        } else {
            stats.async_layout_rejections =
                saturating_add(stats.async_layout_rejections, 1U);
        }
    });
    return scheduled;
}

bool ZenithTabRuntime::try_take_layout_async(ForegroundLayoutReady* ready) noexcept {
    if (!impl_->opened || !impl_->foreground_registered ||
        impl_->foreground_pool == nullptr || ready == nullptr) {
        return false;
    }
    if (!impl_->foreground_pool->try_take_ready(impl_->session_id, ready)) {
        return false;
    }

    impl_->update_statistics([&](ZenithTabRuntimeStats& stats) {
        stats.async_layout_ready_drains =
            saturating_add(stats.async_layout_ready_drains, 1U);
        if (ready->succeeded) {
            stats.async_layout_success_drains = saturating_add(
                stats.async_layout_success_drains,
                1U);
        } else {
            stats.async_layout_failure_drains = saturating_add(
                stats.async_layout_failure_drains,
                1U);
        }
    });

    if (ready->succeeded &&
        impl_->visibility.load(std::memory_order_acquire) == FrameVisibility::Visible) {
        impl_->scheduler.begin_frame(
            impl_->pressure.load(std::memory_order_acquire),
            FrameVisibility::Visible);
        FrameWorkRequest delivery_charge;
        delivery_charge.work_class = FrameWorkClass::Visible;
        delivery_charge.lane = FrameExecutionLane::Ui;
        delivery_charge.reserve_us = 1U;
        delivery_charge.may_block = false;
        static_cast<void>(impl_->scheduler.reserve(delivery_charge));
        impl_->scheduler.finish_visible_phase();
        impl_->schedule_prefetch(ready->result);
    }
    return true;
}

bool ZenithTabRuntime::async_layout_enabled() const noexcept {
    return impl_->opened && impl_->foreground_registered &&
           impl_->foreground_pool != nullptr;
}

FrameVisibility ZenithTabRuntime::visibility() const noexcept {
    return impl_->visibility.load(std::memory_order_acquire);
}

FramePressure ZenithTabRuntime::pressure() const noexcept {
    return impl_->pressure.load(std::memory_order_acquire);
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
