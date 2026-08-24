#include "foreground_layout_worker_pool.hpp"

#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zevryon::massivedoc {
namespace {

constexpr std::size_t kMaximumForegroundLayoutWorkers = 64U;

std::uint64_t saturating_increment(std::uint64_t value) noexcept {
    return value == std::numeric_limits<std::uint64_t>::max() ? value : value + 1U;
}

ForegroundLayoutWorkerScheduleResult map_schedule_result(
    ForegroundLayoutScheduleResult result) noexcept {
    switch (result) {
    case ForegroundLayoutScheduleResult::Accepted:
        return ForegroundLayoutWorkerScheduleResult::Accepted;
    case ForegroundLayoutScheduleResult::Coalesced:
        return ForegroundLayoutWorkerScheduleResult::Coalesced;
    case ForegroundLayoutScheduleResult::Replaced:
        return ForegroundLayoutWorkerScheduleResult::Replaced;
    case ForegroundLayoutScheduleResult::Stale:
        return ForegroundLayoutWorkerScheduleResult::Stale;
    case ForegroundLayoutScheduleResult::Inactive:
        return ForegroundLayoutWorkerScheduleResult::Inactive;
    case ForegroundLayoutScheduleResult::UnknownSession:
        return ForegroundLayoutWorkerScheduleResult::UnknownSession;
    case ForegroundLayoutScheduleResult::Invalid:
        return ForegroundLayoutWorkerScheduleResult::Invalid;
    case ForegroundLayoutScheduleResult::Stopped:
        return ForegroundLayoutWorkerScheduleResult::Stopped;
    }
    return ForegroundLayoutWorkerScheduleResult::Invalid;
}

bool schedules_work(ForegroundLayoutScheduleResult result) noexcept {
    return result == ForegroundLayoutScheduleResult::Accepted ||
           result == ForegroundLayoutScheduleResult::Replaced;
}

} // namespace

bool ForegroundLayoutWorkerPoolConfig::valid() const noexcept {
    return worker_count != 0U && worker_count <= kMaximumForegroundLayoutWorkers &&
           handoff.valid();
}

struct SharedForegroundLayoutWorkerPool::State {
    struct Session {
        ForegroundLayoutExecutor executor;
        bool active{true};
        bool queued{false};
        bool running{false};
        bool rerun_requested{false};
        std::uint64_t latest_request_id{0U};
    };

    explicit State(ForegroundLayoutWorkerPoolConfig config_value)
        : config(config_value), handoff(config_value.handoff) {}

    ForegroundLayoutWorkerPoolConfig config;
    SharedForegroundLayoutHandoff handoff;
    mutable std::mutex mutex;
    std::condition_variable work_cv;
    std::condition_variable idle_cv;
    std::unordered_map<std::uint64_t, Session> sessions;
    std::deque<std::uint64_t> queue;
    std::vector<std::thread> workers;
    std::uint64_t thread_starts{0U};
    std::uint64_t thread_start_failures{0U};
    std::uint64_t runs_started{0U};
    std::uint64_t runs_succeeded{0U};
    std::uint64_t runs_failed{0U};
    std::uint64_t runs_not_claimed{0U};
    bool stopped{false};

    bool ensure_workers_locked() noexcept {
        try {
            while (workers.size() < config.worker_count) {
                workers.emplace_back([this] { worker_loop(); });
                thread_starts = saturating_increment(thread_starts);
            }
            return true;
        } catch (...) {
            thread_start_failures = saturating_increment(thread_start_failures);
            return false;
        }
    }

    void enqueue_locked(std::uint64_t session_id, Session* session) {
        if (session == nullptr || session->queued || !session->active || stopped) {
            return;
        }
        queue.push_back(session_id);
        session->queued = true;
        work_cv.notify_one();
    }

    void finish_run(
        std::uint64_t session_id,
        std::uint64_t claimed_request_id,
        bool claimed,
        bool execution_succeeded) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = sessions.find(session_id);
        if (claimed) {
            if (execution_succeeded) {
                runs_succeeded = saturating_increment(runs_succeeded);
            } else {
                runs_failed = saturating_increment(runs_failed);
            }
        } else {
            runs_not_claimed = saturating_increment(runs_not_claimed);
        }
        if (found != sessions.end()) {
            Session& session = found->second;
            session.running = false;
            const bool newer_request_waits =
                session.rerun_requested && session.active && !stopped &&
                session.latest_request_id > claimed_request_id;
            session.rerun_requested = false;
            if (newer_request_waits) {
                enqueue_locked(session_id, &session);
            }
        }
        idle_cv.notify_all();
    }

    void worker_loop() {
        for (;;) {
            std::uint64_t session_id = 0U;
            ForegroundLayoutExecutor executor;
            {
                std::unique_lock<std::mutex> lock(mutex);
                work_cv.wait(lock, [this] { return stopped || !queue.empty(); });
                if (stopped && queue.empty()) {
                    return;
                }
                session_id = queue.front();
                queue.pop_front();
                const auto found = sessions.find(session_id);
                if (found == sessions.end()) {
                    idle_cv.notify_all();
                    continue;
                }
                Session& session = found->second;
                session.queued = false;
                if (!session.active || session.running || !session.executor) {
                    idle_cv.notify_all();
                    continue;
                }
                session.running = true;
                executor = session.executor;
                runs_started = saturating_increment(runs_started);
            }

            ForegroundLayoutRequest request;
            if (!handoff.try_take_pending(session_id, &request)) {
                finish_run(session_id, 0U, false, false);
                continue;
            }

            ForegroundLayoutReady ready;
            ready.request_id = request.request_id;
            bool used_checkpoint_path = false;
            std::string error;
            const bool execution_succeeded = executor(
                request,
                &ready.result,
                &used_checkpoint_path,
                &error);
            ready.request_id = request.request_id;
            ready.used_checkpoint_path = used_checkpoint_path;
            ready.succeeded = execution_succeeded;
            ready.error = std::move(error);
            static_cast<void>(handoff.publish_ready(session_id, std::move(ready)));
            finish_run(
                session_id,
                request.request_id,
                true,
                execution_succeeded);
        }
    }
};

SharedForegroundLayoutWorkerPool::SharedForegroundLayoutWorkerPool(
    ForegroundLayoutWorkerPoolConfig config)
    : state_(std::make_unique<State>(config)) {}

SharedForegroundLayoutWorkerPool::~SharedForegroundLayoutWorkerPool() {
    stop();
}

bool SharedForegroundLayoutWorkerPool::valid() const noexcept {
    return state_ != nullptr && state_->config.valid() && state_->handoff.valid();
}

bool SharedForegroundLayoutWorkerPool::open_session(
    std::uint64_t session_id,
    ForegroundLayoutExecutor executor,
    bool active) {
    if (!valid() || session_id == 0U || !executor) {
        return false;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->stopped || state_->sessions.contains(session_id) ||
        !state_->ensure_workers_locked()) {
        return false;
    }
    if (!state_->handoff.open_session(session_id, active)) {
        return false;
    }

    State::Session session;
    session.executor = std::move(executor);
    session.active = active;
    const auto inserted = state_->sessions.emplace(session_id, std::move(session));
    if (!inserted.second) {
        static_cast<void>(state_->handoff.close_session(session_id));
        return false;
    }
    return true;
}

bool SharedForegroundLayoutWorkerPool::close_session(std::uint64_t session_id) {
    if (!valid() || session_id == 0U) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto found = state_->sessions.find(session_id);
        if (found == state_->sessions.end()) {
            return false;
        }
        found->second.active = false;
        found->second.rerun_requested = false;
    }
    static_cast<void>(state_->handoff.set_session_active(session_id, false));

    {
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->idle_cv.wait(lock, [this, session_id] {
            const auto found = state_->sessions.find(session_id);
            return found == state_->sessions.end() || !found->second.running;
        });
        const auto found = state_->sessions.find(session_id);
        if (found == state_->sessions.end()) {
            return false;
        }
        state_->sessions.erase(found);
        state_->idle_cv.notify_all();
    }
    return state_->handoff.close_session(session_id);
}

bool SharedForegroundLayoutWorkerPool::set_session_active(
    std::uint64_t session_id,
    bool active) {
    if (!valid() || session_id == 0U) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto found = state_->sessions.find(session_id);
    if (state_->stopped || found == state_->sessions.end()) {
        return false;
    }
    State::Session& session = found->second;
    if (!active) {
        session.active = false;
        session.rerun_requested = false;
        return state_->handoff.set_session_active(session_id, false);
    }
    if (!state_->handoff.set_session_active(session_id, true)) {
        return false;
    }
    session.active = true;
    return true;
}

ForegroundLayoutWorkerScheduleResult SharedForegroundLayoutWorkerPool::request(
    std::uint64_t session_id,
    ForegroundLayoutRequest request) {
    if (!valid()) {
        return ForegroundLayoutWorkerScheduleResult::Invalid;
    }
    const std::uint64_t request_id = request.request_id;
    const ForegroundLayoutScheduleResult scheduled =
        state_->handoff.schedule(session_id, std::move(request));
    if (!schedules_work(scheduled)) {
        return map_schedule_result(scheduled);
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto found = state_->sessions.find(session_id);
    if (state_->stopped) {
        return ForegroundLayoutWorkerScheduleResult::Stopped;
    }
    if (found == state_->sessions.end()) {
        return ForegroundLayoutWorkerScheduleResult::UnknownSession;
    }
    State::Session& session = found->second;
    if (!session.active) {
        return ForegroundLayoutWorkerScheduleResult::Inactive;
    }
    session.latest_request_id = request_id;
    if (session.running) {
        session.rerun_requested = true;
    } else {
        state_->enqueue_locked(session_id, &session);
    }
    return map_schedule_result(scheduled);
}

bool SharedForegroundLayoutWorkerPool::try_take_ready(
    std::uint64_t session_id,
    ForegroundLayoutReady* ready) {
    if (!valid()) {
        return false;
    }
    return state_->handoff.try_take_ready(session_id, ready);
}

bool SharedForegroundLayoutWorkerPool::wait_idle_for(
    std::chrono::milliseconds timeout) {
    if (!valid() || timeout.count() < 0) {
        return false;
    }
    std::unique_lock<std::mutex> lock(state_->mutex);
    return state_->idle_cv.wait_for(lock, timeout, [this] {
        if (!state_->queue.empty()) {
            return false;
        }
        for (const auto& item : state_->sessions) {
            const State::Session& session = item.second;
            if (session.queued || session.running || session.rerun_requested) {
                return false;
            }
        }
        return true;
    });
}

ForegroundLayoutWorkerPoolStatus SharedForegroundLayoutWorkerPool::status() const {
    ForegroundLayoutWorkerPoolStatus result;
    if (!valid()) {
        return result;
    }
    result.handoff = state_->handoff.status();
    std::lock_guard<std::mutex> lock(state_->mutex);
    result.configured_workers = state_->config.worker_count;
    result.live_threads = state_->workers.size();
    result.sessions = state_->sessions.size();
    for (const auto& item : state_->sessions) {
        const State::Session& session = item.second;
        result.active_sessions += session.active ? 1U : 0U;
        result.queued_sessions += session.queued ? 1U : 0U;
        result.running_sessions += session.running ? 1U : 0U;
    }
    result.thread_starts = state_->thread_starts;
    result.thread_start_failures = state_->thread_start_failures;
    result.runs_started = state_->runs_started;
    result.runs_succeeded = state_->runs_succeeded;
    result.runs_failed = state_->runs_failed;
    result.runs_not_claimed = state_->runs_not_claimed;
    result.stopped = state_->stopped;
    return result;
}

void SharedForegroundLayoutWorkerPool::stop() {
    if (!valid()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->stopped) {
            return;
        }
        state_->stopped = true;
        state_->queue.clear();
        for (auto& item : state_->sessions) {
            item.second.active = false;
            item.second.queued = false;
            item.second.rerun_requested = false;
        }
        state_->work_cv.notify_all();
    }

    state_->handoff.stop();
    for (std::thread& worker : state_->workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->workers.clear();
        state_->sessions.clear();
        state_->idle_cv.notify_all();
    }
}

} // namespace zevryon::massivedoc
