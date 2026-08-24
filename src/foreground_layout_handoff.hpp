#pragma once

#include "layout_window.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace zevryon::massivedoc {

struct ForegroundLayoutHandoffConfig {
    std::size_t max_ready_bytes{4U * 1024U * 1024U};
    std::size_t max_fragments_per_request{4096U};

    bool valid() const noexcept;
};

struct ForegroundLayoutRequest {
    // Caller-owned, strictly increasing identity within one session.
    // Zero is invalid. Newer identities obsolete older pending/running/ready work.
    std::uint64_t request_id{0U};
    std::uint64_t scroll_y_q8{0U};
    std::uint32_t viewport_width_q8{0U};
    std::uint64_t viewport_height_q8{0U};
    std::uint64_t overscan_q8{0U};
    std::size_t max_fragments{0U};

    bool operator==(const ForegroundLayoutRequest&) const noexcept = default;
};

struct ForegroundLayoutReady {
    std::uint64_t request_id{0U};
    LayoutWindowResult result{};
    bool used_checkpoint_path{false};
    bool succeeded{false};
    std::string error;
};

enum class ForegroundLayoutScheduleResult : std::uint8_t {
    Accepted = 0U,
    Coalesced,
    Replaced,
    Stale,
    Inactive,
    UnknownSession,
    Invalid,
    Stopped,
};

struct ForegroundLayoutHandoffStatus {
    std::size_t sessions{0U};
    std::size_t active_sessions{0U};
    std::size_t pending_sessions{0U};
    std::size_t running_sessions{0U};
    std::size_t ready_results{0U};
    std::size_t ready_bytes{0U};
    std::size_t ready_peak_bytes{0U};
    std::uint64_t requests_total{0U};
    std::uint64_t requests_accepted{0U};
    std::uint64_t requests_coalesced{0U};
    std::uint64_t requests_replaced{0U};
    std::uint64_t requests_stale{0U};
    std::uint64_t requests_inactive{0U};
    std::uint64_t requests_unknown_session{0U};
    std::uint64_t requests_invalid{0U};
    std::uint64_t requests_stopped{0U};
    std::uint64_t pending_invalidations{0U};
    std::uint64_t ready_invalidations{0U};
    std::uint64_t stale_publications{0U};
    std::uint64_t ready_budget_drops{0U};
    std::uint64_t ready_replacements{0U};
    bool stopped{false};
};

// Process-shared bounded mailbox between a UI-side request producer and
// worker-side synchronous layout execution. This class does not own worker
// threads; it defines the concurrency-safe ownership and staleness contract.
class SharedForegroundLayoutHandoff final {
public:
    explicit SharedForegroundLayoutHandoff(
        ForegroundLayoutHandoffConfig config = {});
    ~SharedForegroundLayoutHandoff();

    SharedForegroundLayoutHandoff(const SharedForegroundLayoutHandoff&) = delete;
    SharedForegroundLayoutHandoff& operator=(const SharedForegroundLayoutHandoff&) = delete;
    SharedForegroundLayoutHandoff(SharedForegroundLayoutHandoff&&) = delete;
    SharedForegroundLayoutHandoff& operator=(SharedForegroundLayoutHandoff&&) = delete;

    bool valid() const noexcept;
    bool open_session(std::uint64_t session_id, bool active = true);
    bool close_session(std::uint64_t session_id);
    bool set_session_active(std::uint64_t session_id, bool active);

    ForegroundLayoutScheduleResult schedule(
        std::uint64_t session_id,
        ForegroundLayoutRequest request);

    // Worker side: claims the latest pending request for the session. At most
    // one request per session can be running at a time.
    bool try_take_pending(
        std::uint64_t session_id,
        ForegroundLayoutRequest* request);

    // Worker side: publishes the result only if it is still the newest request
    // authority for an active session and fits the global ready-byte budget.
    bool publish_ready(
        std::uint64_t session_id,
        ForegroundLayoutReady ready);

    // UI side: moves out the newest ready result for the session.
    bool try_take_ready(
        std::uint64_t session_id,
        ForegroundLayoutReady* ready);

    ForegroundLayoutHandoffStatus status() const;
    void stop();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace zevryon::massivedoc
