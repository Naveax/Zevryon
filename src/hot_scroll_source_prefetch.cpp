#include "hot_scroll_source_prefetch.hpp"

#include "massivedoc_store.hpp"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace zevryon::massivedoc {

struct SourceWindowPrefetchWorker::State {
    State(
        std::filesystem::path root_value,
        SourceWindowPrefetchExecutor executor_value)
        : store_root(std::move(root_value)),
          store(store_root),
          executor(std::move(executor_value)) {}

    bool execute(
        const SourceWindowPrefetchRequest& request,
        std::vector<std::byte>* bytes,
        std::string* error) {
        if (executor) {
            return executor(store_root, request, bytes, error);
        }
        if (!store_opened) {
            if (!store.open(error)) {
                return false;
            }
            store_opened = true;
        }
        return store.read_record_slice(
            request.record_index,
            request.byte_offset,
            request.max_bytes,
            bytes,
            error);
    }

    void run() {
        for (;;) {
            std::unique_lock<std::mutex> lock(mutex);
            work_cv.wait(lock, [this] { return stop_requested || pending.has_value(); });
            if (stop_requested) {
                pending.reset();
                idle_cv.notify_all();
                return;
            }

            SourceWindowPrefetchRequest request_value = *pending;
            pending.reset();
            running = true;
            ++runs_started;
            lock.unlock();

            std::vector<std::byte> bytes;
            std::string error;
            bool succeeded = false;
            try {
                succeeded = execute(request_value, &bytes, &error);
            } catch (const std::exception& exception) {
                error = std::string("source-window prefetch executor threw: ") +
                        exception.what();
            } catch (...) {
                error = "source-window prefetch executor threw";
            }
            if (!succeeded && error.empty()) {
                error = "source-window prefetch failed without diagnostic";
            }

            lock.lock();
            running = false;
            if (succeeded) {
                ++runs_succeeded;
            } else {
                ++runs_failed;
            }

            if (stop_requested || request_value.ticket.direction == 0 ||
                request_value.ticket != authority) {
                ++stale_results_dropped;
            } else {
                SourceWindowPrefetchResult published;
                published.request = request_value;
                published.bytes = std::move(bytes);
                published.succeeded = succeeded;
                published.error = std::move(error);
                if (ready.has_value()) {
                    ++ready_replacements;
                }
                ready = std::move(published);
            }
            idle_cv.notify_all();

            if (stop_requested) {
                pending.reset();
                idle_cv.notify_all();
                return;
            }
        }
    }

    std::filesystem::path store_root;
    StoreReader store;
    bool store_opened{false};
    SourceWindowPrefetchExecutor executor;

    mutable std::mutex mutex;
    std::condition_variable work_cv;
    std::condition_variable idle_cv;
    std::thread worker;
    PrefetchTicket authority{};
    std::optional<SourceWindowPrefetchRequest> pending;
    std::optional<SourceWindowPrefetchResult> ready;
    bool stop_requested{false};
    bool running{false};

    std::uint64_t requests_total{0U};
    std::uint64_t requests_accepted{0U};
    std::uint64_t requests_coalesced{0U};
    std::uint64_t requests_replaced{0U};
    std::uint64_t requests_stale{0U};
    std::uint64_t requests_invalid{0U};
    std::uint64_t requests_stopped{0U};
    std::uint64_t pending_cancellations{0U};
    std::uint64_t ready_invalidations{0U};
    std::uint64_t runs_started{0U};
    std::uint64_t runs_succeeded{0U};
    std::uint64_t runs_failed{0U};
    std::uint64_t stale_results_dropped{0U};
    std::uint64_t ready_replacements{0U};
};

SourceWindowPrefetchWorker::SourceWindowPrefetchWorker(
    std::filesystem::path store_root)
    : SourceWindowPrefetchWorker(std::move(store_root), {}) {}

SourceWindowPrefetchWorker::SourceWindowPrefetchWorker(
    std::filesystem::path store_root,
    SourceWindowPrefetchExecutor executor)
    : state_(std::make_unique<State>(std::move(store_root), std::move(executor))) {
    State* const state = state_.get();
    state->worker = std::thread([state] { state->run(); });
}

SourceWindowPrefetchWorker::~SourceWindowPrefetchWorker() {
    try {
        stop();
    } catch (...) {
        std::terminate();
    }
}

void SourceWindowPrefetchWorker::set_authority_ticket(PrefetchTicket ticket) {
    State* const state = state_.get();
    std::unique_lock<std::mutex> lock(state->mutex);
    if (ticket == state->authority) {
        return;
    }
    state->authority = ticket;
    if (state->pending.has_value() && state->pending->ticket != ticket) {
        state->pending.reset();
        ++state->pending_cancellations;
    }
    if (state->ready.has_value() && state->ready->request.ticket != ticket) {
        state->ready.reset();
        ++state->ready_invalidations;
    }
    lock.unlock();
    state->idle_cv.notify_all();
}

PrefetchTicket SourceWindowPrefetchWorker::authority_ticket() const {
    const State* const state = state_.get();
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->authority;
}

SourceWindowPrefetchScheduleResult SourceWindowPrefetchWorker::request(
    SourceWindowPrefetchRequest request_value) {
    State* const state = state_.get();
    std::unique_lock<std::mutex> lock(state->mutex);
    ++state->requests_total;

    if (state->stop_requested) {
        ++state->requests_stopped;
        return SourceWindowPrefetchScheduleResult::stopped;
    }
    if (request_value.max_bytes == 0U || request_value.max_bytes > kIoWindowBytes) {
        ++state->requests_invalid;
        return SourceWindowPrefetchScheduleResult::invalid;
    }
    if (request_value.ticket.direction == 0 || request_value.ticket != state->authority) {
        ++state->requests_stale;
        return SourceWindowPrefetchScheduleResult::stale;
    }
    if (state->ready.has_value() && state->ready->request == request_value) {
        ++state->requests_coalesced;
        return SourceWindowPrefetchScheduleResult::coalesced;
    }
    if (state->pending.has_value()) {
        if (*state->pending == request_value) {
            ++state->requests_coalesced;
            return SourceWindowPrefetchScheduleResult::coalesced;
        }
        state->pending = request_value;
        ++state->requests_replaced;
        lock.unlock();
        state->work_cv.notify_one();
        return SourceWindowPrefetchScheduleResult::replaced;
    }

    state->pending = request_value;
    ++state->requests_accepted;
    lock.unlock();
    state->work_cv.notify_one();
    return SourceWindowPrefetchScheduleResult::accepted;
}

bool SourceWindowPrefetchWorker::try_take_ready(SourceWindowPrefetchResult* result) {
    if (result == nullptr) {
        return false;
    }
    State* const state = state_.get();
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->ready.has_value()) {
        return false;
    }
    *result = std::move(*state->ready);
    state->ready.reset();
    return true;
}

bool SourceWindowPrefetchWorker::wait_idle_for(std::chrono::milliseconds timeout) {
    State* const state = state_.get();
    std::unique_lock<std::mutex> lock(state->mutex);
    return state->idle_cv.wait_for(
        lock,
        timeout,
        [state] { return !state->running && !state->pending.has_value(); });
}

SourceWindowPrefetchStatus SourceWindowPrefetchWorker::status() const {
    const State* const state = state_.get();
    std::lock_guard<std::mutex> lock(state->mutex);
    SourceWindowPrefetchStatus snapshot;
    snapshot.authority_ticket = state->authority;
    snapshot.running = state->running;
    snapshot.pending = state->pending.has_value();
    snapshot.ready = state->ready.has_value();
    snapshot.stopped = state->stop_requested;
    snapshot.requests_total = state->requests_total;
    snapshot.requests_accepted = state->requests_accepted;
    snapshot.requests_coalesced = state->requests_coalesced;
    snapshot.requests_replaced = state->requests_replaced;
    snapshot.requests_stale = state->requests_stale;
    snapshot.requests_invalid = state->requests_invalid;
    snapshot.requests_stopped = state->requests_stopped;
    snapshot.pending_cancellations = state->pending_cancellations;
    snapshot.ready_invalidations = state->ready_invalidations;
    snapshot.runs_started = state->runs_started;
    snapshot.runs_succeeded = state->runs_succeeded;
    snapshot.runs_failed = state->runs_failed;
    snapshot.stale_results_dropped = state->stale_results_dropped;
    snapshot.ready_replacements = state->ready_replacements;
    return snapshot;
}

void SourceWindowPrefetchWorker::stop() {
    State* const state = state_.get();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->stop_requested) {
            return;
        }
        state->stop_requested = true;
        if (state->pending.has_value()) {
            state->pending.reset();
            ++state->pending_cancellations;
        }
    }
    state->work_cv.notify_all();
    state->idle_cv.notify_all();
    if (state->worker.joinable() &&
        state->worker.get_id() != std::this_thread::get_id()) {
        state->worker.join();
    }
}

} // namespace zevryon::massivedoc
