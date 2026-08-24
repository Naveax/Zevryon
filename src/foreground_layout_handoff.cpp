#include "foreground_layout_handoff.hpp"

#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace zevryon::massivedoc {
namespace {

std::uint64_t saturating_increment(std::uint64_t value) noexcept {
    return value == std::numeric_limits<std::uint64_t>::max() ? value : value + 1U;
}

std::size_t saturating_add(std::size_t left, std::size_t right) noexcept {
    return left > std::numeric_limits<std::size_t>::max() - right
               ? std::numeric_limits<std::size_t>::max()
               : left + right;
}

std::size_t ready_charge(const ForegroundLayoutReady& ready) noexcept {
    std::size_t charge = sizeof(ForegroundLayoutReady);
    if (ready.result.fragments.capacity() >
        std::numeric_limits<std::size_t>::max() / sizeof(LayoutFragment)) {
        return std::numeric_limits<std::size_t>::max();
    }
    charge = saturating_add(
        charge,
        ready.result.fragments.capacity() * sizeof(LayoutFragment));
    charge = saturating_add(charge, ready.error.capacity());
    return charge;
}

bool valid_request(
    const ForegroundLayoutHandoffConfig& config,
    const ForegroundLayoutRequest& request) noexcept {
    return request.request_id != 0U && request.viewport_width_q8 != 0U &&
           request.viewport_height_q8 != 0U && request.max_fragments != 0U &&
           request.max_fragments <= config.max_fragments_per_request;
}

} // namespace

bool ForegroundLayoutHandoffConfig::valid() const noexcept {
    return max_ready_bytes != 0U && max_fragments_per_request != 0U;
}

struct SharedForegroundLayoutHandoff::State {
    struct Session {
        bool active{true};
        bool has_latest_request{false};
        ForegroundLayoutRequest latest_request{};
        std::optional<ForegroundLayoutRequest> pending;
        std::optional<ForegroundLayoutRequest> running;
        std::optional<ForegroundLayoutReady> ready;
        std::size_t ready_charge_bytes{0U};
    };

    explicit State(ForegroundLayoutHandoffConfig config_value)
        : config(config_value) {}

    ForegroundLayoutHandoffConfig config;
    mutable std::mutex mutex;
    std::unordered_map<std::uint64_t, Session> sessions;
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

    void remove_ready(Session* session, bool count_invalidation) noexcept {
        if (session == nullptr || !session->ready.has_value()) {
            return;
        }
        ready_bytes = session->ready_charge_bytes <= ready_bytes
                          ? ready_bytes - session->ready_charge_bytes
                          : 0U;
        session->ready.reset();
        session->ready_charge_bytes = 0U;
        if (count_invalidation) {
            ready_invalidations = saturating_increment(ready_invalidations);
        }
    }

    void remove_pending(Session* session, bool count_invalidation) noexcept {
        if (session == nullptr || !session->pending.has_value()) {
            return;
        }
        session->pending.reset();
        if (count_invalidation) {
            pending_invalidations = saturating_increment(pending_invalidations);
        }
    }
};

SharedForegroundLayoutHandoff::SharedForegroundLayoutHandoff(
    ForegroundLayoutHandoffConfig config)
    : state_(std::make_unique<State>(config)) {}

SharedForegroundLayoutHandoff::~SharedForegroundLayoutHandoff() = default;

bool SharedForegroundLayoutHandoff::valid() const noexcept {
    return state_ != nullptr && state_->config.valid();
}

bool SharedForegroundLayoutHandoff::open_session(
    std::uint64_t session_id,
    bool active) {
    if (!valid() || session_id == 0U) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->stopped || state_->sessions.contains(session_id)) {
        return false;
    }
    State::Session session;
    session.active = active;
    return state_->sessions.emplace(session_id, std::move(session)).second;
}

bool SharedForegroundLayoutHandoff::close_session(std::uint64_t session_id) {
    if (!valid() || session_id == 0U) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto found = state_->sessions.find(session_id);
    if (found == state_->sessions.end()) {
        return false;
    }
    state_->remove_ready(&found->second, false);
    state_->sessions.erase(found);
    return true;
}

bool SharedForegroundLayoutHandoff::set_session_active(
    std::uint64_t session_id,
    bool active) {
    if (!valid() || session_id == 0U) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto found = state_->sessions.find(session_id);
    if (found == state_->sessions.end() || state_->stopped) {
        return false;
    }
    State::Session& session = found->second;
    session.active = active;
    if (!active) {
        state_->remove_pending(&session, true);
        state_->remove_ready(&session, true);
    }
    return true;
}

ForegroundLayoutScheduleResult SharedForegroundLayoutHandoff::schedule(
    std::uint64_t session_id,
    ForegroundLayoutRequest request) {
    if (!valid()) {
        return ForegroundLayoutScheduleResult::Invalid;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->requests_total = saturating_increment(state_->requests_total);

    if (!valid_request(state_->config, request) || session_id == 0U) {
        state_->requests_invalid = saturating_increment(state_->requests_invalid);
        return ForegroundLayoutScheduleResult::Invalid;
    }
    if (state_->stopped) {
        state_->requests_stopped = saturating_increment(state_->requests_stopped);
        return ForegroundLayoutScheduleResult::Stopped;
    }
    const auto found = state_->sessions.find(session_id);
    if (found == state_->sessions.end()) {
        state_->requests_unknown_session =
            saturating_increment(state_->requests_unknown_session);
        return ForegroundLayoutScheduleResult::UnknownSession;
    }
    State::Session& session = found->second;
    if (!session.active) {
        state_->requests_inactive = saturating_increment(state_->requests_inactive);
        return ForegroundLayoutScheduleResult::Inactive;
    }

    if (session.has_latest_request) {
        if (request.request_id < session.latest_request.request_id) {
            state_->requests_stale = saturating_increment(state_->requests_stale);
            return ForegroundLayoutScheduleResult::Stale;
        }
        if (request.request_id == session.latest_request.request_id) {
            if (request == session.latest_request) {
                state_->requests_coalesced =
                    saturating_increment(state_->requests_coalesced);
                return ForegroundLayoutScheduleResult::Coalesced;
            }
            state_->requests_invalid = saturating_increment(state_->requests_invalid);
            return ForegroundLayoutScheduleResult::Invalid;
        }
    }

    const bool replaced_pending = session.pending.has_value();
    if (replaced_pending) {
        state_->remove_pending(&session, true);
    }
    state_->remove_ready(&session, true);

    session.has_latest_request = true;
    session.latest_request = request;
    session.pending = std::move(request);

    if (replaced_pending) {
        state_->requests_replaced = saturating_increment(state_->requests_replaced);
        return ForegroundLayoutScheduleResult::Replaced;
    }
    state_->requests_accepted = saturating_increment(state_->requests_accepted);
    return ForegroundLayoutScheduleResult::Accepted;
}

bool SharedForegroundLayoutHandoff::try_take_pending(
    std::uint64_t session_id,
    ForegroundLayoutRequest* request) {
    if (!valid() || request == nullptr || session_id == 0U) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->stopped) {
        return false;
    }
    const auto found = state_->sessions.find(session_id);
    if (found == state_->sessions.end()) {
        return false;
    }
    State::Session& session = found->second;
    if (!session.active || session.running.has_value() || !session.pending.has_value()) {
        return false;
    }
    session.running = std::move(session.pending);
    session.pending.reset();
    *request = *session.running;
    return true;
}

bool SharedForegroundLayoutHandoff::publish_ready(
    std::uint64_t session_id,
    ForegroundLayoutReady ready) {
    if (!valid() || session_id == 0U || ready.request_id == 0U) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->stopped) {
        return false;
    }
    const auto found = state_->sessions.find(session_id);
    if (found == state_->sessions.end()) {
        return false;
    }
    State::Session& session = found->second;
    if (!session.running.has_value() ||
        ready.request_id != session.running->request_id) {
        state_->stale_publications =
            saturating_increment(state_->stale_publications);
        return false;
    }

    session.running.reset();
    if (!session.active || !session.has_latest_request ||
        ready.request_id != session.latest_request.request_id) {
        state_->stale_publications =
            saturating_increment(state_->stale_publications);
        return false;
    }

    const std::size_t charge = ready_charge(ready);
    if (charge > state_->config.max_ready_bytes) {
        state_->ready_budget_drops =
            saturating_increment(state_->ready_budget_drops);
        return false;
    }

    if (session.ready.has_value()) {
        state_->remove_ready(&session, false);
        state_->ready_replacements =
            saturating_increment(state_->ready_replacements);
    }
    if (state_->ready_bytes > state_->config.max_ready_bytes - charge) {
        state_->ready_budget_drops =
            saturating_increment(state_->ready_budget_drops);
        return false;
    }

    session.ready = std::move(ready);
    session.ready_charge_bytes = charge;
    state_->ready_bytes += charge;
    if (state_->ready_bytes > state_->ready_peak_bytes) {
        state_->ready_peak_bytes = state_->ready_bytes;
    }
    return true;
}

bool SharedForegroundLayoutHandoff::try_take_ready(
    std::uint64_t session_id,
    ForegroundLayoutReady* ready) {
    if (!valid() || ready == nullptr || session_id == 0U) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto found = state_->sessions.find(session_id);
    if (found == state_->sessions.end() || !found->second.ready.has_value()) {
        return false;
    }
    State::Session& session = found->second;
    *ready = std::move(*session.ready);
    state_->remove_ready(&session, false);
    return true;
}

ForegroundLayoutHandoffStatus SharedForegroundLayoutHandoff::status() const {
    ForegroundLayoutHandoffStatus result;
    if (!valid()) {
        return result;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    result.sessions = state_->sessions.size();
    for (const auto& item : state_->sessions) {
        const State::Session& session = item.second;
        result.active_sessions += session.active ? 1U : 0U;
        result.pending_sessions += session.pending.has_value() ? 1U : 0U;
        result.running_sessions += session.running.has_value() ? 1U : 0U;
        result.ready_results += session.ready.has_value() ? 1U : 0U;
    }
    result.ready_bytes = state_->ready_bytes;
    result.ready_peak_bytes = state_->ready_peak_bytes;
    result.requests_total = state_->requests_total;
    result.requests_accepted = state_->requests_accepted;
    result.requests_coalesced = state_->requests_coalesced;
    result.requests_replaced = state_->requests_replaced;
    result.requests_stale = state_->requests_stale;
    result.requests_inactive = state_->requests_inactive;
    result.requests_unknown_session = state_->requests_unknown_session;
    result.requests_invalid = state_->requests_invalid;
    result.requests_stopped = state_->requests_stopped;
    result.pending_invalidations = state_->pending_invalidations;
    result.ready_invalidations = state_->ready_invalidations;
    result.stale_publications = state_->stale_publications;
    result.ready_budget_drops = state_->ready_budget_drops;
    result.ready_replacements = state_->ready_replacements;
    result.stopped = state_->stopped;
    return result;
}

void SharedForegroundLayoutHandoff::stop() {
    if (!valid()) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->stopped) {
        return;
    }
    state_->stopped = true;
    for (auto& item : state_->sessions) {
        State::Session& session = item.second;
        state_->remove_pending(&session, true);
        state_->remove_ready(&session, true);
        session.running.reset();
        session.active = false;
    }
}

} // namespace zevryon::massivedoc
