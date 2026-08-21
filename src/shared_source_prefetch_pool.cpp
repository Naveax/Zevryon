#include "shared_source_prefetch_pool.hpp"

#include "massivedoc_store.hpp"

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
        std::unique_ptr<StoreReader> store;
        bool store_opened{false};
    };

    struct QueueEntry {
        std::shared_ptr<SessionState> session;
        std::uint64_t queue_epoch{0U};
    };

    State(
        SharedSourcePrefetchPoolConfig config_value,
        SharedSourcePrefetchExecutor executor_value)
        : config(config_value), executor(std::move(executor_value)) {}

    bool execute(
        const std::shared_ptr<SessionState>& session,
        const SourceWindowPrefetchRequest& request_value,
        std::vector<std::byte>* bytes,
        std::string* error) {
        if (executor) {
            return executor(
                session->root,
                session->id,
                request_value,
                bytes,
                error);
        }
        if (!session->store) {
            session->store = std::make_unique<StoreReader>(session->root);
        }
        if (!session->store_opened) {
            if (!session->store->open(error)) {
                return false;
            }
            session->store_opened = true;
        }
        return session->store->read_record_slice(
            request_value.record_index,
            request_value.byte_offset,
            request_value.max_bytes,
            bytes,
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

            std::vector<std::byte> bytes;
            std::string error;
            bool succeeded = false;
            try {
                succeeded = execute(session, request_value, &bytes, &error);
            } catch (const std::exception& exception) {
                error = std::string("shared source prefetch executor threw: ") +
                        exception.what();
            } catch (...) {
                error = "shared source prefetch executor threw";
            }
            if (!succeeded && error.empty()) {
                error = "shared source prefetch failed without diagnostic";
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                session->running = false;
                if (succeeded) {
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
                } else {
                    const std::size_t charge = bytes.capacity();
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
                        published.request = request_value;
                        published.bytes = std::move(bytes);
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
    SharedSourcePrefetchExecutor executor;
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
    std::uint64_t stale_results_dropped{0U};
    std::uint64_t inactive_results_dropped{0U};
    std::uint64_t closed_results_dropped{0U};
    std::uint64_t ready_replacements{0U};
    std::uint64_t ready_budget_drops{0U};
};

SharedSourcePrefetchPool::SharedSourcePrefetchPool(
    SharedSourcePrefetchPoolConfig config)
    : SharedSourcePrefetchPool(config, {}) {}

SharedSourcePrefetchPool::SharedSourcePrefetchPool(
    SharedSourcePrefetchPoolConfig config,
    SharedSourcePrefetchExecutor executor)
    : state_(std::make_unique<State>(config, std::move(executor))) {}

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
    snapshot.stale_results_dropped = state->stale_results_dropped;
    snapshot.inactive_results_dropped = state->inactive_results_dropped;
    snapshot.closed_results_dropped = state->closed_results_dropped;
    snapshot.ready_replacements = state->ready_replacements;
    snapshot.ready_budget_drops = state->ready_budget_drops;
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
