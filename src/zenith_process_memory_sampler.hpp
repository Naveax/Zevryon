#pragma once

#include "zenith_process_memory_pressure.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace zevryon::massivedoc {

class ZenithProcessTabController;

struct ZenithProcessMemorySamplerConfig {
    std::uint64_t normal_interval_ms{1'000U};
    std::uint64_t elevated_interval_ms{250U};
    std::uint64_t critical_interval_ms{100U};

    bool valid() const noexcept;
};

enum class ZenithProcessMemoryPollResult : std::uint8_t {
    Sampled = 0U,
    Throttled,
    Failed,
};

struct ZenithProcessMemorySamplerStats {
    std::uint64_t polls{0U};
    std::uint64_t samples{0U};
    std::uint64_t throttled{0U};
    std::uint64_t failures{0U};
    std::uint64_t last_sample_monotonic_ms{0U};
    std::uint64_t next_due_monotonic_ms{0U};
    bool has_attempted_sample{false};
};

using ZenithProcessMemorySnapshotProvider = std::function<bool(
    ZenithProcessMemorySnapshot*,
    std::string*)>;

class ZenithProcessMemorySampler final {
public:
    explicit ZenithProcessMemorySampler(
        ZenithProcessMemorySamplerConfig config = {},
        ZenithProcessMemorySnapshotProvider provider = {});

    bool valid() const noexcept;
    ZenithProcessMemoryPollResult poll(
        std::uint64_t monotonic_ms,
        ZenithProcessMemoryPressurePolicy* policy,
        ZenithProcessTabController* controller,
        ZenithProcessMemorySnapshot* captured,
        std::string* error);
    ZenithProcessMemoryPollResult poll_now(
        ZenithProcessMemoryPressurePolicy* policy,
        ZenithProcessTabController* controller,
        ZenithProcessMemorySnapshot* captured,
        std::string* error);
    ZenithProcessMemorySamplerStats stats() const noexcept;
    void reset() noexcept;

private:
    std::uint64_t interval_for(FramePressure pressure) const noexcept;
    void schedule_next(std::uint64_t monotonic_ms, FramePressure pressure) noexcept;

    ZenithProcessMemorySamplerConfig config_;
    ZenithProcessMemorySnapshotProvider provider_;
    ZenithProcessMemorySamplerStats stats_;
};

} // namespace zevryon::massivedoc
