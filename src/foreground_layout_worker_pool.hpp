#pragma once

#include "foreground_layout_handoff.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace zevryon::massivedoc {

struct ForegroundLayoutWorkerPoolConfig {
    std::size_t worker_count{2U};
    ForegroundLayoutHandoffConfig handoff{};

    bool valid() const noexcept;
};

using ForegroundLayoutExecutor = std::function<bool(
    const ForegroundLayoutRequest&,
    LayoutWindowResult*,
    bool*,
    std::string*)>;

enum class ForegroundLayoutWorkerScheduleResult : std::uint8_t {
    Accepted = 0U,
    Coalesced,
    Replaced,
    Stale,
    Inactive,
    UnknownSession,
    Invalid,
    Stopped,
};

struct ForegroundLayoutWorkerPoolStatus {
    std::size_t configured_workers{0U};
    std::size_t live_threads{0U};
    std::size_t sessions{0U};
    std::size_t active_sessions{0U};
    std::size_t queued_sessions{0U};
    std::size_t running_sessions{0U};
    std::uint64_t thread_starts{0U};
    std::uint64_t thread_start_failures{0U};
    std::uint64_t runs_started{0U};
    std::uint64_t runs_succeeded{0U};
    std::uint64_t runs_failed{0U};
    std::uint64_t runs_not_claimed{0U};
    ForegroundLayoutHandoffStatus handoff{};
    bool stopped{false};
};

// Process-shared bounded worker owner for foreground layout. Worker threads are
// created once when the first session is opened, never per tab. The pool uses
// SharedForegroundLayoutHandoff as the request/result authority and permits at
// most one running layout per session.
class SharedForegroundLayoutWorkerPool final {
public:
    explicit SharedForegroundLayoutWorkerPool(
        ForegroundLayoutWorkerPoolConfig config = {});
    ~SharedForegroundLayoutWorkerPool();

    SharedForegroundLayoutWorkerPool(const SharedForegroundLayoutWorkerPool&) = delete;
    SharedForegroundLayoutWorkerPool& operator=(const SharedForegroundLayoutWorkerPool&) = delete;
    SharedForegroundLayoutWorkerPool(SharedForegroundLayoutWorkerPool&&) = delete;
    SharedForegroundLayoutWorkerPool& operator=(SharedForegroundLayoutWorkerPool&&) = delete;

    bool valid() const noexcept;
    bool open_session(
        std::uint64_t session_id,
        ForegroundLayoutExecutor executor,
        bool active = true);
    bool close_session(std::uint64_t session_id);
    bool set_session_active(std::uint64_t session_id, bool active);

    ForegroundLayoutWorkerScheduleResult request(
        std::uint64_t session_id,
        ForegroundLayoutRequest request);
    bool try_take_ready(
        std::uint64_t session_id,
        ForegroundLayoutReady* ready);
    bool wait_idle_for(std::chrono::milliseconds timeout);
    ForegroundLayoutWorkerPoolStatus status() const;
    void stop();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace zevryon::massivedoc
