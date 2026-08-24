#include "zenith_process_memory_sampler.hpp"

#include "zenith_process_tab_controller.hpp"

#include <chrono>
#include <limits>
#include <utility>

namespace zevryon::massivedoc {
namespace {

std::uint64_t saturating_increment(std::uint64_t value) noexcept {
    return value == std::numeric_limits<std::uint64_t>::max() ? value : value + 1U;
}

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
    return left > std::numeric_limits<std::uint64_t>::max() - right
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}

} // namespace

bool ZenithProcessMemorySamplerConfig::valid() const noexcept {
    return critical_interval_ms > 0U &&
           critical_interval_ms <= elevated_interval_ms &&
           elevated_interval_ms <= normal_interval_ms;
}

ZenithProcessMemorySampler::ZenithProcessMemorySampler(
    ZenithProcessMemorySamplerConfig config,
    ZenithProcessMemorySnapshotProvider provider)
    : config_(config),
      provider_(std::move(provider)) {
    if (!provider_) {
        provider_ = capture_zenith_process_memory_snapshot;
    }
}

bool ZenithProcessMemorySampler::valid() const noexcept {
    return config_.valid() && static_cast<bool>(provider_);
}

std::uint64_t ZenithProcessMemorySampler::interval_for(
    FramePressure pressure) const noexcept {
    switch (pressure) {
    case FramePressure::Critical:
        return config_.critical_interval_ms;
    case FramePressure::Elevated:
        return config_.elevated_interval_ms;
    case FramePressure::Normal:
    default:
        return config_.normal_interval_ms;
    }
}

void ZenithProcessMemorySampler::schedule_next(
    std::uint64_t monotonic_ms,
    FramePressure pressure) noexcept {
    stats_.next_due_monotonic_ms =
        saturating_add(monotonic_ms, interval_for(pressure));
}

ZenithProcessMemoryPollResult ZenithProcessMemorySampler::poll(
    std::uint64_t monotonic_ms,
    ZenithProcessMemoryPressurePolicy* policy,
    ZenithProcessTabController* controller,
    ZenithProcessMemorySnapshot* captured,
    std::string* error) {
    stats_.polls = saturating_increment(stats_.polls);
    if (!valid() || policy == nullptr || controller == nullptr || error == nullptr) {
        stats_.failures = saturating_increment(stats_.failures);
        if (error != nullptr) {
            *error = "invalid process memory sampler poll";
        }
        return ZenithProcessMemoryPollResult::Failed;
    }
    error->clear();
    if (stats_.has_attempted_sample &&
        monotonic_ms < stats_.next_due_monotonic_ms) {
        stats_.throttled = saturating_increment(stats_.throttled);
        return ZenithProcessMemoryPollResult::Throttled;
    }

    stats_.has_attempted_sample = true;
    stats_.last_sample_monotonic_ms = monotonic_ms;

    ZenithProcessMemorySnapshot snapshot;
    std::string capture_error;
    if (!provider_(&snapshot, &capture_error)) {
        stats_.failures = saturating_increment(stats_.failures);
        schedule_next(monotonic_ms, policy->pressure());
        *error = capture_error.empty()
                     ? "unable to capture process memory snapshot"
                     : std::move(capture_error);
        return ZenithProcessMemoryPollResult::Failed;
    }

    stats_.samples = saturating_increment(stats_.samples);
    if (captured != nullptr) {
        *captured = snapshot;
    }
    std::string apply_error;
    if (!apply_zenith_process_memory_pressure_snapshot(
            policy,
            controller,
            snapshot,
            &apply_error)) {
        stats_.failures = saturating_increment(stats_.failures);
        schedule_next(monotonic_ms, policy->pressure());
        *error = apply_error.empty()
                     ? "unable to apply process memory pressure snapshot"
                     : std::move(apply_error);
        return ZenithProcessMemoryPollResult::Failed;
    }

    schedule_next(monotonic_ms, policy->pressure());
    return ZenithProcessMemoryPollResult::Sampled;
}

ZenithProcessMemoryPollResult ZenithProcessMemorySampler::poll_now(
    ZenithProcessMemoryPressurePolicy* policy,
    ZenithProcessTabController* controller,
    ZenithProcessMemorySnapshot* captured,
    std::string* error) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    const std::uint64_t monotonic_ms = millis < 0
                                           ? 0U
                                           : static_cast<std::uint64_t>(millis);
    return poll(monotonic_ms, policy, controller, captured, error);
}

ZenithProcessMemorySamplerStats ZenithProcessMemorySampler::stats() const noexcept {
    return stats_;
}

void ZenithProcessMemorySampler::reset() noexcept {
    stats_ = {};
}

} // namespace zevryon::massivedoc
