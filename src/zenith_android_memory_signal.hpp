#pragma once

#include "device_frame_profile.hpp"
#include "frame_budget_scheduler.hpp"

#include <cstdint>
#include <string>

namespace zevryon::massivedoc {

struct ZenithAndroidMemorySignal {
    // Raw ComponentCallbacks2.onTrimMemory(level) value. Use zero when no trim
    // callback is currently being applied.
    std::int32_t trim_level{0};
    // ActivityManager.isLowRamDevice(). This selects the conservative baseline
    // profile but is not itself a transient pressure event.
    bool low_ram_device{false};
    // ActivityManager.MemoryInfo.lowMemory. This is an explicit system pressure
    // signal and maps directly to Critical.
    bool system_low_memory{false};
};

struct ZenithAndroidMemoryDecision {
    DeviceFrameProfile profile{DeviceFrameProfile::MidPhone};
    FramePressure pressure{FramePressure::Normal};
    bool ui_hidden{false};
    bool background_lru{false};
};

bool evaluate_android_memory_signal(
    std::uint64_t total_ram_mib,
    const ZenithAndroidMemorySignal& signal,
    ZenithAndroidMemoryDecision* decision) noexcept;

class ZenithProcessTabController;

bool apply_android_memory_pressure_signal(
    std::uint64_t total_ram_mib,
    const ZenithAndroidMemorySignal& signal,
    ZenithProcessTabController* controller,
    ZenithAndroidMemoryDecision* decision,
    std::string* error);

} // namespace zevryon::massivedoc
