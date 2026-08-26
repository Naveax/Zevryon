#pragma once

#include "frame_budget_scheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace zevryon::massivedoc {

class ZenithTabRuntime;

using ZenithTabActivitySink = std::function<bool(
    FrameVisibility,
    FramePressure,
    std::int64_t,
    std::string*)>;

enum class ZenithProcessPressureSource : std::uint8_t {
    ProcessMemory = 0U,
    PlatformMemory = 1U,
};

struct ZenithProcessTabControllerStats {
    std::size_t registered_tabs{0U};
    std::size_t visible_tabs{0U};
    std::size_t hidden_tabs{0U};
    std::uint64_t registrations{0U};
    std::uint64_t unregistrations{0U};
    std::uint64_t activity_updates{0U};
    std::uint64_t pressure_changes{0U};
    std::uint64_t activity_applications{0U};
    std::uint64_t application_failures{0U};
    std::uint64_t hidden_background_applications{0U};
    std::uint64_t hidden_critical_applications{0U};
    std::uint64_t visible_critical_applications{0U};
    FramePressure global_pressure{FramePressure::Normal};
};

class ZenithProcessTabController final {
public:
    ZenithProcessTabController();
    ~ZenithProcessTabController();

    ZenithProcessTabController(const ZenithProcessTabController&) = delete;
    ZenithProcessTabController& operator=(const ZenithProcessTabController&) = delete;
    ZenithProcessTabController(ZenithProcessTabController&&) = delete;
    ZenithProcessTabController& operator=(ZenithProcessTabController&&) = delete;

    bool register_tab(
        std::uint64_t session_id,
        FrameVisibility visibility,
        std::int64_t scroll_velocity_q8_per_second,
        ZenithTabActivitySink sink,
        std::string* error);
    bool unregister_tab(std::uint64_t session_id) noexcept;
    bool set_tab_activity(
        std::uint64_t session_id,
        FrameVisibility visibility,
        std::int64_t scroll_velocity_q8_per_second,
        std::string* error);

    // Backward-compatible process-memory source update. Platform-owned pressure
    // sources must use set_pressure_source() so independent signals cannot
    // accidentally clear one another.
    bool set_global_pressure(FramePressure pressure, std::string* error);
    bool set_pressure_source(
        ZenithProcessPressureSource source,
        FramePressure pressure,
        std::string* error);

    bool contains(std::uint64_t session_id) const noexcept;
    FramePressure global_pressure() const noexcept;
    FramePressure pressure_source(ZenithProcessPressureSource source) const noexcept;
    ZenithProcessTabControllerStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

ZenithTabActivitySink make_zenith_tab_runtime_activity_sink(
    ZenithTabRuntime* runtime);

} // namespace zevryon::massivedoc
