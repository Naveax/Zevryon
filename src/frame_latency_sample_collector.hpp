#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace zevryon::massivedoc {

struct FrameLatencySampleCollectorConfig {
    std::size_t warmup_samples{120U};
    std::size_t max_samples{10'000U};

    bool valid() const noexcept;
};

struct FrameLatencySampleCollectorStatus {
    std::uint64_t observations{0U};
    std::uint64_t warmup_discarded{0U};
    std::uint64_t recorded{0U};
    std::uint64_t capacity_drops{0U};
};

class FrameLatencySampleCollector final {
public:
    explicit FrameLatencySampleCollector(
        FrameLatencySampleCollectorConfig config = {});

    bool valid() const noexcept;
    bool observe(std::chrono::nanoseconds elapsed) noexcept;
    void reset() noexcept;

    const std::vector<std::uint64_t>& samples_ns() const noexcept;
    FrameLatencySampleCollectorStatus status() const noexcept;

private:
    FrameLatencySampleCollectorConfig config_;
    std::vector<std::uint64_t> samples_ns_;
    FrameLatencySampleCollectorStatus status_;
};

} // namespace zevryon::massivedoc
