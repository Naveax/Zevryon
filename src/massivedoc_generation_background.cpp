#include "massivedoc_generation_background.hpp"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace zevryon::massivedoc {
namespace {

bool run_default_compaction(
    const std::filesystem::path& store_root,
    GenerationCompactionConfig config,
    GenerationCompactionResult* result,
    std::string* error) {
    return compact_store_generation_metadata(
        store_root,
        config,
        GenerationCompactionCut::none,
        result,
        error);
}

} // namespace

struct GenerationCompactionWorker::State {
    State(
        std::filesystem::path root,
        GenerationCompactionConfig compaction_config,
        GenerationCompactionExecutor compaction_executor)
        : store_root(std::move(root)),
          config(compaction_config),
          executor(std::move(compaction_executor)) {}

    void run() {
        for (;;) {
            std::unique_lock<std::mutex> lock(mutex);
            work_cv.wait(lock, [this] { return stop_requested || pending; });
            if (stop_requested) {
                pending = false;
                idle_cv.notify_all();
                return;
            }

            pending = false;
            running = true;
            ++runs_started;
            lock.unlock();

            GenerationCompactionResult result;
            std::string error;
            bool succeeded = false;
            try {
                succeeded = executor(store_root, config, &result, &error);
            } catch (const std::exception& exception) {
                error = std::string("generation background compaction executor threw: ") +
                        exception.what();
            } catch (...) {
                error = "generation background compaction executor threw";
            }
            if (!succeeded && error.empty()) {
                error = "generation background compaction executor failed without diagnostic";
            }

            lock.lock();
            running = false;
            last_result = result;
            last_error = std::move(error);
            if (succeeded) {
                ++runs_succeeded;
            } else {
                ++runs_failed;
            }
            idle_cv.notify_all();
            if (stop_requested) {
                pending = false;
                idle_cv.notify_all();
                return;
            }
        }
    }

    std::filesystem::path store_root;
    GenerationCompactionConfig config;
    GenerationCompactionExecutor executor;
    mutable std::mutex mutex;
    std::condition_variable work_cv;
    std::condition_variable idle_cv;
    std::thread worker;
    bool stop_requested{false};
    bool running{false};
    bool pending{false};
    std::uint64_t requests_total{0U};
    std::uint64_t requests_accepted{0U};
    std::uint64_t requests_coalesced{0U};
    std::uint64_t requests_stopped{0U};
    std::uint64_t pending_cancellations{0U};
    std::uint64_t runs_started{0U};
    std::uint64_t runs_succeeded{0U};
    std::uint64_t runs_failed{0U};
    GenerationCompactionResult last_result;
    std::string last_error;
};

GenerationCompactionWorker::GenerationCompactionWorker(
    std::filesystem::path store_root,
    GenerationCompactionConfig config)
    : GenerationCompactionWorker(
          std::move(store_root),
          config,
          GenerationCompactionExecutor(run_default_compaction)) {}

GenerationCompactionWorker::GenerationCompactionWorker(
    std::filesystem::path store_root,
    GenerationCompactionConfig config,
    GenerationCompactionExecutor executor)
    : state_(std::make_unique<State>(
          std::move(store_root),
          config,
          executor ? std::move(executor)
                   : GenerationCompactionExecutor(run_default_compaction))) {
    State* const state = state_.get();
    state->worker = std::thread([state] { state->run(); });
}

GenerationCompactionWorker::~GenerationCompactionWorker() {
    try {
        stop();
    } catch (...) {
        std::terminate();
    }
}

GenerationCompactionScheduleResult GenerationCompactionWorker::request() {
    State* const state = state_.get();
    std::unique_lock<std::mutex> lock(state->mutex);
    ++state->requests_total;
    if (state->stop_requested) {
        ++state->requests_stopped;
        return GenerationCompactionScheduleResult::stopped;
    }
    if (state->pending) {
        ++state->requests_coalesced;
        return GenerationCompactionScheduleResult::coalesced;
    }
    state->pending = true;
    ++state->requests_accepted;
    lock.unlock();
    state->work_cv.notify_one();
    return GenerationCompactionScheduleResult::accepted;
}

bool GenerationCompactionWorker::cancel_pending() {
    State* const state = state_.get();
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->pending) {
        return false;
    }
    state->pending = false;
    ++state->pending_cancellations;
    lock.unlock();
    state->idle_cv.notify_all();
    return true;
}

bool GenerationCompactionWorker::wait_idle_for(std::chrono::milliseconds timeout) {
    State* const state = state_.get();
    std::unique_lock<std::mutex> lock(state->mutex);
    return state->idle_cv.wait_for(
        lock,
        timeout,
        [state] { return !state->running && !state->pending; });
}

GenerationBackgroundCompactionStatus GenerationCompactionWorker::status() const {
    const State* const state = state_.get();
    std::lock_guard<std::mutex> lock(state->mutex);
    GenerationBackgroundCompactionStatus snapshot;
    snapshot.running = state->running;
    snapshot.pending = state->pending;
    snapshot.stopped = state->stop_requested;
    snapshot.requests_total = state->requests_total;
    snapshot.requests_accepted = state->requests_accepted;
    snapshot.requests_coalesced = state->requests_coalesced;
    snapshot.requests_stopped = state->requests_stopped;
    snapshot.pending_cancellations = state->pending_cancellations;
    snapshot.runs_started = state->runs_started;
    snapshot.runs_succeeded = state->runs_succeeded;
    snapshot.runs_failed = state->runs_failed;
    snapshot.last_result = state->last_result;
    snapshot.last_error = state->last_error;
    return snapshot;
}

void GenerationCompactionWorker::stop() {
    State* const state = state_.get();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stop_requested = true;
        state->pending = false;
    }
    state->work_cv.notify_all();
    state->idle_cv.notify_all();
    if (state->worker.joinable() &&
        state->worker.get_id() != std::this_thread::get_id()) {
        state->worker.join();
    }
}

} // namespace zevryon::massivedoc
