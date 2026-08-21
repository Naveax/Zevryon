#include "frame_latency_sample_collector.hpp"

#include <limits>

namespace zevryon::massivedoc {
namespace {

std::uint64_t saturating_increment(std::uint64_t value) noexcept {
    return value == std::numeric_limits<std::uint64_t>::max() ? value : value + 1U;
}

constexpr std::size_t kMaximumRetainedSamples = 1'000'000U;
constexpr std::size_t kMaximumWarmupSamples = 1'000'000U;

} // namespace

bool FrameLatencySampleCollectorConfig::valid() const noexcept {
    return max_samples > 0U && max_samples <= kMaximumRetainedSamples &&
           warmup_samples <= kMaximumWarmupSamples;
}

FrameLatencySampleCollector::FrameLatencySampleCollector(
    FrameLatencySampleCollectorConfig config)
    : config_(config) {
    if (config_.valid()) {
        samples_ns_.reserve(config_.max_samples);
    }
}

bool FrameLatencySampleCollector::valid() const noexcept {
    return config_.valid();
}

bool FrameLatencySampleCollector::observe(std::chrono::nanoseconds elapsed) noexcept {
    status_.observations = saturating_increment(status_.observations);
    if (!valid() || elapsed.count() < 0) {
        status_.capacity_drops = saturating_increment(status_.capacity_drops);
        return false;
    }
    if (status_.observations <= static_cast<std::uint64_t>(config_.warmup_samples)) {
        status_.warmup_discarded = saturating_increment(status_.warmup_discarded);
        return false;
    }
    if (samples_ns_.size() >= config_.max_samples) {
        status_.capacity_drops = saturating_increment(status_.capacity_drops);
        return false;
    }
    samples_ns_.push_back(static_cast<std::uint64_t>(elapsed.count()));
    status_.recorded = saturating_increment(status_.recorded);
    return true;
}

void FrameLatencySampleCollector::reset() noexcept {
    samples_ns_.clear();
    status_ = {};
}

const std::vector<std::uint64_t>& FrameLatencySampleCollector::samples_ns() const noexcept {
    return samples_ns_;
}

FrameLatencySampleCollectorStatus FrameLatencySampleCollector::status() const noexcept {
    return status_;
}

} // namespace zevryon::massivedoc
