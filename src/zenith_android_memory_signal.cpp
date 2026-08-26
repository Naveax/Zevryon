#include "zenith_android_memory_signal.hpp"

#include "zenith_process_tab_controller.hpp"

namespace zevryon::massivedoc {
namespace {

// ComponentCallbacks2 values. Modern Android (API 34+) no longer delivers the
// RUNNING_* levels, but accepting them keeps older platform shells compatible.
constexpr std::int32_t kTrimRunningModerate = 5;
constexpr std::int32_t kTrimRunningCritical = 15;
constexpr std::int32_t kTrimUiHidden = 20;
constexpr std::int32_t kTrimBackground = 40;

} // namespace

bool evaluate_android_memory_signal(
    std::uint64_t total_ram_mib,
    const ZenithAndroidMemorySignal& signal,
    ZenithAndroidMemoryDecision* decision) noexcept {
    if (decision == nullptr || total_ram_mib == 0U || signal.trim_level < 0) {
        return false;
    }

    ZenithAndroidMemoryDecision result;
    result.profile = signal.low_ram_device
                         ? DeviceFrameProfile::LegacyPhone
                         : select_device_frame_profile(total_ram_mib);
    result.ui_hidden = signal.trim_level >= kTrimUiHidden;
    result.background_lru = signal.trim_level >= kTrimBackground;

    if (signal.system_low_memory ||
        signal.trim_level >= kTrimBackground ||
        (signal.trim_level >= kTrimRunningCritical &&
         signal.trim_level < kTrimUiHidden)) {
        result.pressure = FramePressure::Critical;
    } else if (signal.trim_level >= kTrimUiHidden ||
               signal.trim_level >= kTrimRunningModerate) {
        result.pressure = FramePressure::Elevated;
    }

    *decision = result;
    return true;
}

bool apply_android_memory_pressure_signal(
    std::uint64_t total_ram_mib,
    const ZenithAndroidMemorySignal& signal,
    ZenithProcessTabController* controller,
    ZenithAndroidMemoryDecision* decision,
    std::string* error) {
    if (controller == nullptr || error == nullptr) {
        if (error != nullptr) {
            *error = "invalid Android memory pressure bridge";
        }
        return false;
    }
    error->clear();

    ZenithAndroidMemoryDecision evaluated;
    if (!evaluate_android_memory_signal(total_ram_mib, signal, &evaluated)) {
        *error = "invalid Android memory signal";
        return false;
    }
    if (decision != nullptr) {
        *decision = evaluated;
    }

    return controller->set_pressure_source(
        ZenithProcessPressureSource::PlatformMemory,
        evaluated.pressure,
        error);
}

} // namespace zevryon::massivedoc
