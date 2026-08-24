    if (session->ready.has_value() &&
        (!active || session->ready->request.ticket != authority_ticket)) {
        state->remove_ready_locked(session);
        state->ready_invalidations = saturating_increment(state->ready_invalidations);
    }
    if (!active) {
        state->invalidate_queue_locked(session);
    }
    state->idle_cv.notify_all();
    state->work_cv.notify_all();
    return true;
}

SharedSourcePrefetchScheduleResult SharedSourcePrefetchPool::request(
    std::uint64_t session_id,
    SourceWindowPrefetchRequest request_value) {
    State* const state = state_.get();
    std::unique_lock<std::mutex> lock(state->mutex);
    state->requests_total = saturating_increment(state->requests_total);

    if (state->stop_requested) {
        state->requests_stopped = saturating_increment(state->requests_stopped);
        return SharedSourcePrefetchScheduleResult::stopped;
    }
    if (!state->config.valid() || request_value.max_bytes == 0U ||
        request_value.max_bytes > kIoWindowBytes) {
        state->requests_invalid = saturating_increment(state->requests_invalid);
        return SharedSourcePrefetchScheduleResult::invalid;
    }
    const auto found = state->sessions.find(session_id);
    if (found == state->sessions.end() || found->second->closed) {
        state->requests_unknown_session =
            saturating_increment(state->requests_unknown_session);
        return SharedSourcePrefetchScheduleResult::unknown_session;
    }
    const std::shared_ptr<State::SessionState>& session = found->second;
    if (!session->active) {
        state->requests_inactive = saturating_increment(state->requests_inactive);
        return SharedSourcePrefetchScheduleResult::inactive;
    }
    if (request_value.ticket.direction == 0 ||
        request_value.ticket != session->authority) {
        state->requests_stale = saturating_increment(state->requests_stale);
        return SharedSourcePrefetchScheduleResult::stale;
    }
    if (session->ready.has_value() && session->ready->request == request_value) {
        state->requests_coalesced = saturating_increment(state->requests_coalesced);
        return SharedSourcePrefetchScheduleResult::coalesced;
    }
    if (session->pending.has_value()) {
        if (*session->pending == request_value) {
            state->requests_coalesced = saturating_increment(state->requests_coalesced);
            return SharedSourcePrefetchScheduleResult::coalesced;
        }
        session->pending = request_value;
        state->requests_replaced = saturating_increment(state->requests_replaced);
        return SharedSourcePrefetchScheduleResult::replaced;
    }

    if (!state->ensure_workers_locked()) {
        return SharedSourcePrefetchScheduleResult::thread_start_failed;
    }
    session->pending = request_value;
    state->requests_accepted = saturating_increment(state->requests_accepted);
    if (!session->running) {
        state->enqueue_locked(session);
    }
    lock.unlock();
    state->work_cv.notify_one();
    return SharedSourcePrefetchScheduleResult::accepted;
}

bool SharedSourcePrefetchPool::try_take_ready(
    std::uint64_t session_id,
    SourceWindowPrefetchResult* result) {
    if (result == nullptr) {
        return false;
    }
    State* const state = state_.get();
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto found = state->sessions.find(session_id);
    if (found == state->sessions.end() || !found->second->ready.has_value()) {
        return false;
    }
    *result = std::move(*found->second->ready);
    state->remove_ready_locked(found->second);
    return true;
}

bool SharedSourcePrefetchPool::wait_idle_for(std::chrono::milliseconds timeout) {
    State* const state = state_.get();
    std::unique_lock<std::mutex> lock(state->mutex);
    return state->idle_cv.wait_for(
        lock,
        timeout,
        [state] { return state->stop_requested || state->idle_locked(); });
}

SharedSourcePrefetchPoolStatus SharedSourcePrefetchPool::status() const {
    const State* const state = state_.get();
    std::lock_guard<std::mutex> lock(state->mutex);
    SharedSourcePrefetchPoolStatus snapshot;
    snapshot.configured_workers = state->config.worker_count;
    snapshot.live_threads = state->workers.size();
    snapshot.sessions = state->sessions.size();
    snapshot.ready_bytes = state->ready_bytes;
    snapshot.ready_peak_bytes = state->ready_peak_bytes;
    snapshot.maximum_queue_depth = state->maximum_queue_depth;
    snapshot.active_sessions = state->active_session_count;
    snapshot.queued_sessions = state->queued_session_count;
    snapshot.running_sessions = state->running_session_count;
    snapshot.ready_results = state->ready_result_count;
    snapshot.thread_starts = state->thread_starts;
    snapshot.thread_start_failures = state->thread_start_failures;
    snapshot.requests_total = state->requests_total;
    snapshot.requests_accepted = state->requests_accepted;
    snapshot.requests_coalesced = state->requests_coalesced;
    snapshot.requests_replaced = state->requests_replaced;
    snapshot.requests_stale = state->requests_stale;
    snapshot.requests_inactive = state->requests_inactive;
    snapshot.requests_unknown_session = state->requests_unknown_session;
    snapshot.requests_invalid = state->requests_invalid;
    snapshot.requests_stopped = state->requests_stopped;
    snapshot.pending_cancellations = state->pending_cancellations;
    snapshot.ready_invalidations = state->ready_invalidations;
    snapshot.fairness_requeues = state->fairness_requeues;
    snapshot.runs_started = state->runs_started;
    snapshot.runs_succeeded = state->runs_succeeded;
    snapshot.runs_failed = state->runs_failed;
    snapshot.runs_suppressed = state->runs_suppressed;
    snapshot.stale_results_dropped = state->stale_results_dropped;
    snapshot.inactive_results_dropped = state->inactive_results_dropped;
    snapshot.closed_results_dropped = state->closed_results_dropped;
    snapshot.ready_replacements = state->ready_replacements;
    snapshot.ready_budget_drops = state->ready_budget_drops;
    snapshot.canonicalized_results = state->canonicalized_results;
    snapshot.worker_eof_suppressions = state->worker_eof_suppressions;
    snapshot.record_length_resolve_failures = state->record_length_resolve_failures;
    snapshot.record_length_learns = state->record_length_learns;
    snapshot.stopped = state->stop_requested;
    return snapshot;
}

void SharedSourcePrefetchPool::stop() {
    State* const state = state_.get();
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->stop_requested && state->workers.empty()) {
            return;
        }
        state->stop_requested = true;
        for (auto& [id, session] : state->sessions) {
            static_cast<void>(id);
            session->closed = true;
            session->pending.reset();
            state->invalidate_queue_locked(session);
            state->remove_ready_locked(session);
            if (session->counted) {
                if (session->active && state->active_session_count != 0U) {
                    --state->active_session_count;
                }
                if (session->running && state->running_session_count != 0U) {
                    --state->running_session_count;
                }
                session->counted = false;
            }
            session->active = false;
        }
        state->queue.clear();
        workers.swap(state->workers);
    }
    state->work_cv.notify_all();
    state->idle_cv.notify_all();
    for (std::thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->sessions.clear();
        state->ready_bytes = 0U;
        state->active_session_count = 0U;
        state->queued_session_count = 0U;
        state->running_session_count = 0U;
        state->ready_result_count = 0U;
    }
    state->idle_cv.notify_all();
}

} // namespace zevryon::massivedoc
