                    const std::size_t existing = session->ready_charge_bytes;
                    const std::size_t base_ready =
                        existing <= ready_bytes ? ready_bytes - existing : 0U;
                    if (charge > config.max_ready_bytes ||
                        base_ready > config.max_ready_bytes - charge) {
                        ready_budget_drops = saturating_increment(ready_budget_drops);
                    } else {
                        if (session->ready.has_value()) {
                            ready_replacements = saturating_increment(ready_replacements);
                            remove_ready_locked(session);
                        }
                        SourceWindowPrefetchResult published;
                        published.request = canonical;
                        published.bytes = std::move(execution.bytes);
                        published.succeeded = succeeded;
                        published.error = std::move(error);
                        session->ready = std::move(published);
                        if (session->counted) {
                            ++ready_result_count;
                        }
                        session->ready_charge_bytes = charge;
                        ready_bytes += charge;
                        if (ready_bytes > ready_peak_bytes) {
                            ready_peak_bytes = ready_bytes;
                        }
                    }
                }

                if (still_registered) {
                    requeue_after_run_locked(session);
                }
                idle_cv.notify_all();
                work_cv.notify_all();
            }
        }
    }

    SharedSourcePrefetchPoolConfig config;
    SharedSourcePrefetchExecutor legacy_executor;
    SharedSourcePrefetchExecutorV2 executor_v2;
    mutable std::mutex mutex;
    std::condition_variable work_cv;
    std::condition_variable idle_cv;
    std::vector<std::thread> workers;
    std::unordered_map<std::uint64_t, std::shared_ptr<SessionState>> sessions;
    std::deque<QueueEntry> queue;
    bool stop_requested{false};
    std::size_t ready_bytes{0U};
    std::size_t ready_peak_bytes{0U};
    std::size_t maximum_queue_depth{0U};
    std::size_t active_session_count{0U};
    std::size_t queued_session_count{0U};
    std::size_t running_session_count{0U};
    std::size_t ready_result_count{0U};
    std::uint64_t thread_starts{0U};
    std::uint64_t thread_start_failures{0U};
    std::uint64_t requests_total{0U};
    std::uint64_t requests_accepted{0U};
    std::uint64_t requests_coalesced{0U};
    std::uint64_t requests_replaced{0U};
    std::uint64_t requests_stale{0U};
    std::uint64_t requests_inactive{0U};
    std::uint64_t requests_unknown_session{0U};
    std::uint64_t requests_invalid{0U};
    std::uint64_t requests_stopped{0U};
    std::uint64_t pending_cancellations{0U};
    std::uint64_t ready_invalidations{0U};
    std::uint64_t fairness_requeues{0U};
    std::uint64_t runs_started{0U};
    std::uint64_t runs_succeeded{0U};
    std::uint64_t runs_failed{0U};
    std::uint64_t runs_suppressed{0U};
    std::uint64_t stale_results_dropped{0U};
    std::uint64_t inactive_results_dropped{0U};
    std::uint64_t closed_results_dropped{0U};
    std::uint64_t ready_replacements{0U};
    std::uint64_t ready_budget_drops{0U};
    std::uint64_t canonicalized_results{0U};
    std::uint64_t worker_eof_suppressions{0U};
    std::uint64_t record_length_resolve_failures{0U};
    std::uint64_t record_length_learns{0U};
};

SharedSourcePrefetchPool::SharedSourcePrefetchPool(
    SharedSourcePrefetchPoolConfig config)
    : state_(std::make_unique<State>(
          config,
          SharedSourcePrefetchExecutor{},
          SharedSourcePrefetchExecutorV2{})) {}

SharedSourcePrefetchPool::SharedSourcePrefetchPool(
    SharedSourcePrefetchPoolConfig config,
    SharedSourcePrefetchExecutor executor)
    : state_(std::make_unique<State>(
          config,
          std::move(executor),
          SharedSourcePrefetchExecutorV2{})) {}

SharedSourcePrefetchPool::SharedSourcePrefetchPool(
    SharedSourcePrefetchPoolConfig config,
    SharedSourcePrefetchExecutorV2 executor)
    : state_(std::make_unique<State>(
          config,
          SharedSourcePrefetchExecutor{},
          std::move(executor))) {}

SharedSourcePrefetchPool::~SharedSourcePrefetchPool() {
    stop();
}

bool SharedSourcePrefetchPool::valid() const noexcept {
    return state_->config.valid();
}

bool SharedSourcePrefetchPool::open_session(
    std::uint64_t session_id,
    std::filesystem::path store_root,
    PrefetchTicket authority_ticket,
    bool active) {
    State* const state = state_.get();
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->stop_requested || !state->config.valid() ||
        state->sessions.find(session_id) != state->sessions.end() ||
        state->sessions.size() >= state->config.max_sessions) {
        return false;
    }
    state->sessions.emplace(
        session_id,
        std::make_shared<State::SessionState>(
            session_id,
            std::move(store_root),
            authority_ticket,
            active));
    if (active) {
        ++state->active_session_count;
    }
    return true;
}

bool SharedSourcePrefetchPool::close_session(std::uint64_t session_id) {
    State* const state = state_.get();
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto found = state->sessions.find(session_id);
    if (found == state->sessions.end()) {
        return false;
    }
    const std::shared_ptr<State::SessionState> session = found->second;
    session->closed = true;
    if (session->pending.has_value()) {
        session->pending.reset();
        state->pending_cancellations = saturating_increment(state->pending_cancellations);
    }
    state->invalidate_queue_locked(session);
    if (session->ready.has_value()) {
        state->remove_ready_locked(session);
        state->ready_invalidations = saturating_increment(state->ready_invalidations);
    }
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
    state->sessions.erase(found);
    state->idle_cv.notify_all();
    state->work_cv.notify_all();
    return true;
}

bool SharedSourcePrefetchPool::set_session_authority(
    std::uint64_t session_id,
    PrefetchTicket authority_ticket,
    bool active) {
    State* const state = state_.get();
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto found = state->sessions.find(session_id);
    if (found == state->sessions.end() || found->second->closed) {
        return false;
    }
    const std::shared_ptr<State::SessionState>& session = found->second;
    session->authority = authority_ticket;
    if (session->counted && session->active != active) {
        if (active) {
            ++state->active_session_count;
        } else if (state->active_session_count != 0U) {
            --state->active_session_count;
        }
    }
    session->active = active;

    if (session->pending.has_value() &&
        (!active || session->pending->ticket != authority_ticket)) {
        session->pending.reset();
        state->pending_cancellations = saturating_increment(state->pending_cancellations);
        state->invalidate_queue_locked(session);
    }
