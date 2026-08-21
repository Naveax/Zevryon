#include "shared_source_prefetch_pool.hpp"

#include "massivedoc_store.hpp"
#include "prefetch_record_bounds.hpp"
#include "shared_record_length_authority.hpp"
#include "store_record_length_probe.hpp"

#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>

namespace zevryon::massivedoc {
namespace {

std::uint64_t saturating_increment(std::uint64_t value) noexcept {
    return value == UINT64_MAX ? value : value + 1U;
}

bool canonical_execution_valid(
    const SourceWindowPrefetchRequest& original,
    const SharedSourcePrefetchExecution& execution,
    bool succeeded) noexcept {
    const SourceWindowPrefetchRequest& canonical = execution.canonical_request;
    if (canonical.record_index != original.record_index ||
        canonical.ticket != original.ticket) {
        return false;
    }
    if (execution.suppressed) {
        return succeeded && execution.bytes.empty();
    }
    if (canonical.max_bytes == 0U ||
        canonical.max_bytes > original.max_bytes ||
        canonical.max_bytes > kIoWindowBytes) {
        return false;
    }
    return !succeeded || execution.bytes.size() <= canonical.max_bytes;
}

} // namespace

bool SharedSourcePrefetchPoolConfig::valid() const noexcept {
    return worker_count > 0U && worker_count <= 64U && max_sessions > 0U &&
           max_ready_bytes > 0U;
}

struct SharedSourcePrefetchPool::State {
    struct SessionState {
        SessionState(
            std::uint64_t id_value,
            std::filesystem::path root_value,
            PrefetchTicket authority_value,
            bool active_value)
            : id(id_value),
              root(std::move(root_value)),
              authority(authority_value),
              active(active_value) {}

        std::uint64_t id{0U};
        std::filesystem::path root;
        PrefetchTicket authority{};
        bool active{true};
        bool closed{false};
        bool queued{false};
        bool running{false};
        std::uint64_t queue_epoch{0U};
        std::optional<SourceWindowPrefetchRequest> pending;
        std::optional<SourceWindowPrefetchResult> ready;
        std::size_t ready_charge_bytes{0U};
    };

    struct QueueEntry {
        std::shared_ptr<SessionState> session;
        std::uint64_t queue_epoch{0U};
    };

    State(
        SharedSourcePrefetchPoolConfig config_value,
        SharedSourcePrefetchExecutor legacy_executor_value,
        SharedSourcePrefetchExecutorV2 executor_v2_value)
        : config(config_value),
          legacy_executor(std::move(legacy_executor_value)),
          executor_v2(std::move(executor_v2_value)) {}

    bool execute_default(
        const std::shared_ptr<SessionState>& session,
        const SourceWindowPrefetchRequest& request_value,
        SharedSourcePrefetchExecution* execution,
        std::string* error) {
        execution->canonical_request = request_value;

        if (config.record_length_authority != nullptr &&
            request_value.has_visible_edge_offset) {
            std::uint64_t record_length = 0U;
            std::string metadata_error;
            const bool resolved = config.record_length_authority->query(
                session->root,
                request_value.record_index,
                [](const std::filesystem::path& root,
                   std::uint64_t record_index,
                   std::uint64_t* length,
                   std::string* resolver_error) {
                    return probe_store_record_length(
                        root,
                        record_index,
                        length,
                        resolver_error);
                },
                &record_length,
                &metadata_error);
            if (resolved) {
                const RecordBoundPrefetchDecision decision =
                    clamp_prefetch_to_record(
                        request_value.ticket.direction,
                        request_value.visible_edge_offset,
                        request_value.byte_offset,
                        request_value.max_bytes,
                        record_length);
                if (!decision.should_issue) {
                    execution->canonical_request.byte_offset =
                        decision.byte_offset;
                    execution->canonical_request.max_bytes = 0U;
                    execution->suppressed = decision.eof_suppressed;
                    error->clear();
                    return decision.eof_suppressed;
                }
                execution->canonical_request.byte_offset =
                    decision.byte_offset;
                execution->canonical_request.max_bytes =
                    decision.request_bytes;
            } else {
                execution->record_length_resolve_failed = true;
            }
        }

        // Reader lifetime is one worker execution, not one browser tab. The
        // number of concurrently live default readers is therefore bounded by
        // worker_count rather than session cardinality.
        StoreReader store(session->root);
        if (!store.open(error)) {
            return false;
        }
        const SourceWindowPrefetchRequest& canonical =
            execution->canonical_request;
        if (!store.read_record_slice(
                canonical.record_index,
                canonical.byte_offset,
                canonical.max_bytes,
                &execution->bytes,
                error)) {
            return false;
        }

        if (config.record_length_authority != nullptr &&
            execution->bytes.size() < canonical.max_bytes &&
            canonical.byte_offset <= UINT64_MAX - execution->bytes.size()) {
            const std::uint64_t learned_length =
                canonical.byte_offset +
                static_cast<std::uint64_t>(execution->bytes.size());
            std::string learn_error;
            if (config.record_length_authority->remember(
                    session->root,
                    canonical.record_index,
                    learned_length,
                    &learn_error)) {
                execution->record_length_learned = true;
            }
        }
        error->clear();
        return true;
    }

    bool execute(
        const std::shared_ptr<SessionState>& session,
        const SourceWindowPrefetchRequest& request_value,
        SharedSourcePrefetchExecution* execution,
        std::string* error) {
        if (execution == nullptr || error == nullptr) {
            return false;
        }
        *execution = {};
        execution->canonical_request = request_value;

        if (executor_v2) {
            return executor_v2.run(
                session->root,
                session->id,
                request_value,
                execution,
                error);
        }
        if (legacy_executor) {
            const bool succeeded = legacy_executor(
                session->root,
                session->id,
                request_value,
                &execution->bytes,
                error);
            execution->canonical_request = request_value;
            return succeeded;
        }
        return execute_default(
            session,
            request_value,
            execution,
            error);
    }

    void remove_ready_locked(const std::shared_ptr<SessionState>& session) noexcept {
        if (!session->ready.has_value()) {
            return;
        }
        if (session->ready_charge_bytes <= ready_bytes) {
            ready_bytes -= session->ready_charge_bytes;
        } else {
            ready_bytes = 0U;
        }
        session->ready.reset();
        session->ready_charge_bytes = 0U;
    }

    void invalidate_queue_locked(const std::shared_ptr<SessionState>& session) noexcept {
        if (!session->queued) {
            return;
        }
        session->queued = false;
        session->queue_epoch = saturating_increment(session->queue_epoch);
    }

    void enqueue_locked(const std::shared_ptr<SessionState>& session) {
        if (session->queued || session->closed || !session->active ||
            !session->pending.has_value()) {
            return;
        }
        session->queue_epoch = saturating_increment(session->queue_epoch);
        session->queued = true;
        queue.push_back(QueueEntry{session, session->queue_epoch});
        if (queue.size() > maximum_queue_depth) {
            maximum_queue_depth = queue.size();
        }
    }

    bool ensure_workers_locked() noexcept {
        if (stop_requested) {
            return false;
        }
        while (workers.size() < config.worker_count) {
            try {
                State* const self = this;
                workers.emplace_back([self] { self->run(); });
                thread_starts = saturating_increment(thread_starts);
            } catch (...) {
                thread_start_failures = saturating_increment(thread_start_failures);
                break;
            }
        }
        return !workers.empty();
    }

    bool idle_locked() const noexcept {
        for (const auto& [id, session] : sessions) {
            static_cast<void>(id);
            if (session->running || session->queued || session->pending.has_value()) {
                return false;
            }
        }
        return true;
    }

    void requeue_after_run_locked(const std::shared_ptr<SessionState>& session) {
        if (!session->closed && session->active && session->pending.has_value()) {
            enqueue_locked(session);
            fairness_requeues = saturating_increment(fairness_requeues);
        }
    }

    void run() {
        for (;;) {
            std::shared_ptr<SessionState> session;
            SourceWindowPrefetchRequest request_value;

            {
                std::unique_lock<std::mutex> lock(mutex);
                work_cv.wait(lock, [this] { return stop_requested || !queue.empty(); });
                if (stop_requested) {
                    idle_cv.notify_all();
                    return;
                }

                while (!queue.empty()) {
                    QueueEntry entry = std::move(queue.front());
                    queue.pop_front();
                    if (!entry.session || entry.session->closed ||
                        !entry.session->queued ||
                        entry.queue_epoch != entry.session->queue_epoch) {
                        continue;
                    }
                    entry.session->queued = false;
                    if (!entry.session->active || !entry.session->pending.has_value()) {
                        continue;
                    }
                    session = std::move(entry.session);
                    request_value = *session->pending;
                    session->pending.reset();
                    session->running = true;
                    runs_started = saturating_increment(runs_started);
                    break;
                }
                if (!session) {
                    idle_cv.notify_all();
                    continue;
                }
            }

            SharedSourcePrefetchExecution execution;
            std::string error;
            bool succeeded = false;
            try {
                succeeded = execute(
                    session,
                    request_value,
                    &execution,
                    &error);
            } catch (const std::exception& exception) {
                error = std::string("shared source prefetch executor threw: ") +
                        exception.what();
            } catch (...) {
                error = "shared source prefetch executor threw";
            }

            if (!canonical_execution_valid(request_value, execution, succeeded)) {
                succeeded = false;
                execution = {};
                execution.canonical_request = request_value;
                error = "shared source prefetch executor returned invalid canonical request";
            } else if (!succeeded && error.empty()) {
                error = "shared source prefetch failed without diagnostic";
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                session->running = false;
                if (execution.record_length_resolve_failed) {
                    record_length_resolve_failures =
                        saturating_increment(record_length_resolve_failures);
                }
                if (execution.record_length_learned) {
                    record_length_learns =
                        saturating_increment(record_length_learns);
                }

                if (execution.suppressed) {
                    worker_eof_suppressions =
                        saturating_increment(worker_eof_suppressions);
                    runs_suppressed = saturating_increment(runs_suppressed);
                } else if (succeeded) {
                    runs_succeeded = saturating_increment(runs_succeeded);
                } else {
                    runs_failed = saturating_increment(runs_failed);
                }

                const auto found = sessions.find(session->id);
                const bool still_registered =
                    found != sessions.end() && found->second.get() == session.get();
                if (!still_registered || session->closed) {
                    closed_results_dropped = saturating_increment(closed_results_dropped);
                } else if (!session->active) {
                    inactive_results_dropped = saturating_increment(inactive_results_dropped);
                } else if (request_value.ticket.direction == 0 ||
                           request_value.ticket != session->authority) {
                    stale_results_dropped = saturating_increment(stale_results_dropped);
                } else if (!execution.suppressed) {
                    const SourceWindowPrefetchRequest& canonical =
                        execution.canonical_request;
                    if (!(canonical == request_value)) {
                        canonicalized_results =
                            saturating_increment(canonicalized_results);
                    }
                    const std::size_t charge = execution.bytes.capacity();
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
    session->active = false;
    if (session->pending.has_value()) {
        session->pending.reset();
        state->pending_cancellations = saturating_increment(state->pending_cancellations);
    }
    state->invalidate_queue_locked(session);
    if (session->ready.has_value()) {
        state->remove_ready_locked(session);
        state->ready_invalidations = saturating_increment(state->ready_invalidations);
    }
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
    session->active = active;

    if (session->pending.has_value() &&
        (!active || session->pending->ticket != authority_ticket)) {
        session->pending.reset();
        state->pending_cancellations = saturating_increment(state->pending_cancellations);
        state->invalidate_queue_locked(session);
    }
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
    for (const auto& [id, session] : state->sessions) {
        static_cast<void>(id);
        snapshot.active_sessions += session->active ? 1U : 0U;
        snapshot.queued_sessions += session->queued ? 1U : 0U;
        snapshot.running_sessions += session->running ? 1U : 0U;
        snapshot.ready_results += session->ready.has_value() ? 1U : 0U;
    }
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
            session->active = false;
            session->pending.reset();
            session->queued = false;
            state->remove_ready_locked(session);
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
    }
    state->idle_cv.notify_all();
}

} // namespace zevryon::massivedoc
