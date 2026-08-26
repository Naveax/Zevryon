#include "zenith_process_tab_controller.hpp"

#include "zenith_tab_runtime.hpp"

#include <array>
#include <exception>
#include <limits>
#include <unordered_map>
#include <utility>

namespace zevryon::massivedoc {
namespace {

constexpr std::size_t kPressureSourceCount = 2U;

std::uint64_t saturating_increment(std::uint64_t value) noexcept {
    return value == std::numeric_limits<std::uint64_t>::max() ? value : value + 1U;
}

std::size_t pressure_source_index(ZenithProcessPressureSource source) noexcept {
    switch (source) {
    case ZenithProcessPressureSource::ProcessMemory:
        return 0U;
    case ZenithProcessPressureSource::PlatformMemory:
        return 1U;
    }
    return kPressureSourceCount;
}

unsigned int pressure_rank(FramePressure pressure) noexcept {
    switch (pressure) {
    case FramePressure::Critical:
        return 2U;
    case FramePressure::Elevated:
        return 1U;
    case FramePressure::Normal:
    default:
        return 0U;
    }
}

FramePressure stronger_pressure(FramePressure left, FramePressure right) noexcept {
    return pressure_rank(left) >= pressure_rank(right) ? left : right;
}

} // namespace

struct ZenithProcessTabController::Impl {
    struct Entry {
        FrameVisibility visibility{FrameVisibility::Hidden};
        std::int64_t scroll_velocity_q8_per_second{0};
        ZenithTabActivitySink sink;
    };

    std::unordered_map<std::uint64_t, Entry> entries;
    std::size_t visible_tabs{0U};
    std::size_t hidden_tabs{0U};
    std::array<FramePressure, kPressureSourceCount> pressure_sources{
        FramePressure::Normal,
        FramePressure::Normal};
    FramePressure pressure{FramePressure::Normal};
    ZenithProcessTabControllerStats statistics;

    FramePressure combined_pressure() const noexcept {
        return stronger_pressure(pressure_sources[0U], pressure_sources[1U]);
    }

    void sync_current_counts() noexcept {
        statistics.registered_tabs = entries.size();
        statistics.visible_tabs = visible_tabs;
        statistics.hidden_tabs = hidden_tabs;
        statistics.global_pressure = pressure;
    }

    void add_visibility(FrameVisibility visibility) noexcept {
        if (visibility == FrameVisibility::Visible) {
            ++visible_tabs;
        } else {
            ++hidden_tabs;
        }
    }

    void remove_visibility(FrameVisibility visibility) noexcept {
        if (visibility == FrameVisibility::Visible) {
            if (visible_tabs > 0U) {
                --visible_tabs;
            }
        } else if (hidden_tabs > 0U) {
            --hidden_tabs;
        }
    }

    bool apply(
        Entry& entry,
        FramePressure target_pressure,
        std::string* error) noexcept {
        statistics.activity_applications =
            saturating_increment(statistics.activity_applications);
        if (entry.visibility == FrameVisibility::Hidden) {
            if (target_pressure == FramePressure::Critical) {
                statistics.hidden_critical_applications =
                    saturating_increment(statistics.hidden_critical_applications);
            } else {
                statistics.hidden_background_applications =
                    saturating_increment(statistics.hidden_background_applications);
            }
        } else if (target_pressure == FramePressure::Critical) {
            statistics.visible_critical_applications =
                saturating_increment(statistics.visible_critical_applications);
        }

        try {
            std::string local_error;
            const bool ok = entry.sink(
                entry.visibility,
                target_pressure,
                entry.visibility == FrameVisibility::Visible
                    ? entry.scroll_velocity_q8_per_second
                    : 0,
                &local_error);
            if (!ok) {
                statistics.application_failures =
                    saturating_increment(statistics.application_failures);
                if (error != nullptr) {
                    *error = local_error.empty()
                                 ? "tab activity sink rejected process policy"
                                 : std::move(local_error);
                }
                return false;
            }
            return true;
        } catch (const std::exception& exception) {
            statistics.application_failures =
                saturating_increment(statistics.application_failures);
            if (error != nullptr) {
                *error = std::string("tab activity sink threw: ") + exception.what();
            }
            return false;
        } catch (...) {
            statistics.application_failures =
                saturating_increment(statistics.application_failures);
            if (error != nullptr) {
                *error = "tab activity sink threw";
            }
            return false;
        }
    }
};

ZenithProcessTabController::ZenithProcessTabController()
    : impl_(std::make_unique<Impl>()) {
    impl_->sync_current_counts();
}

ZenithProcessTabController::~ZenithProcessTabController() = default;

bool ZenithProcessTabController::register_tab(
    std::uint64_t session_id,
    FrameVisibility visibility,
    std::int64_t scroll_velocity_q8_per_second,
    ZenithTabActivitySink sink,
    std::string* error) {
    if (error == nullptr || !sink) {
        if (error != nullptr) {
            *error = "invalid process tab registration";
        }
        return false;
    }
    error->clear();
    if (impl_->entries.find(session_id) != impl_->entries.end()) {
        *error = "process tab session is already registered";
        return false;
    }

    try {
        Impl::Entry entry;
        entry.visibility = visibility;
        entry.scroll_velocity_q8_per_second =
            visibility == FrameVisibility::Visible
                ? scroll_velocity_q8_per_second
                : 0;
        entry.sink = std::move(sink);
        const auto inserted = impl_->entries.emplace(session_id, std::move(entry));
        if (!inserted.second) {
            *error = "process tab session registration failed";
            return false;
        }
        impl_->add_visibility(inserted.first->second.visibility);

        std::string apply_error;
        if (!impl_->apply(inserted.first->second, impl_->pressure, &apply_error)) {
            impl_->remove_visibility(inserted.first->second.visibility);
            impl_->entries.erase(inserted.first);
            impl_->sync_current_counts();
            *error = apply_error.empty()
                         ? "unable to apply initial process tab policy"
                         : std::move(apply_error);
            return false;
        }
        impl_->statistics.registrations =
            saturating_increment(impl_->statistics.registrations);
        impl_->sync_current_counts();
        return true;
    } catch (const std::exception& exception) {
        impl_->sync_current_counts();
        *error = std::string("unable to allocate process tab metadata: ") +
                 exception.what();
        return false;
    } catch (...) {
        impl_->sync_current_counts();
        *error = "unable to allocate process tab metadata";
        return false;
    }
}

bool ZenithProcessTabController::unregister_tab(std::uint64_t session_id) noexcept {
    const auto found = impl_->entries.find(session_id);
    if (found == impl_->entries.end()) {
        return false;
    }
    impl_->remove_visibility(found->second.visibility);
    impl_->entries.erase(found);
    impl_->statistics.unregistrations =
        saturating_increment(impl_->statistics.unregistrations);
    impl_->sync_current_counts();
    return true;
}

bool ZenithProcessTabController::set_tab_activity(
    std::uint64_t session_id,
    FrameVisibility visibility,
    std::int64_t scroll_velocity_q8_per_second,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    const auto found = impl_->entries.find(session_id);
    if (found == impl_->entries.end()) {
        *error = "unknown process tab session";
        return false;
    }

    Impl::Entry& entry = found->second;
    const std::int64_t normalized_velocity =
        visibility == FrameVisibility::Visible
            ? scroll_velocity_q8_per_second
            : 0;
    if (entry.visibility == visibility &&
        entry.scroll_velocity_q8_per_second == normalized_velocity) {
        return true;
    }

    if (entry.visibility != visibility) {
        impl_->remove_visibility(entry.visibility);
        impl_->add_visibility(visibility);
    }
    entry.visibility = visibility;
    entry.scroll_velocity_q8_per_second = normalized_velocity;
    impl_->statistics.activity_updates =
        saturating_increment(impl_->statistics.activity_updates);
    impl_->sync_current_counts();
    return impl_->apply(entry, impl_->pressure, error);
}

bool ZenithProcessTabController::set_global_pressure(
    FramePressure pressure,
    std::string* error) {
    return set_pressure_source(
        ZenithProcessPressureSource::ProcessMemory,
        pressure,
        error);
}

bool ZenithProcessTabController::set_pressure_source(
    ZenithProcessPressureSource source,
    FramePressure pressure,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();

    const std::size_t source_index = pressure_source_index(source);
    if (source_index >= impl_->pressure_sources.size()) {
        *error = "invalid process pressure source";
        return false;
    }
    if (impl_->pressure_sources[source_index] == pressure) {
        return true;
    }

    impl_->pressure_sources[source_index] = pressure;
    const FramePressure target_pressure = impl_->combined_pressure();
    if (target_pressure == impl_->pressure) {
        return true;
    }

    impl_->pressure = target_pressure;
    impl_->statistics.pressure_changes =
        saturating_increment(impl_->statistics.pressure_changes);
    impl_->sync_current_counts();

    bool all_applied = true;
    std::string first_error;
    const auto apply_visibility = [&](FrameVisibility visibility) {
        for (auto& [session_id, entry] : impl_->entries) {
            static_cast<void>(session_id);
            if (entry.visibility != visibility) {
                continue;
            }
            std::string apply_error;
            if (!impl_->apply(entry, target_pressure, &apply_error)) {
                all_applied = false;
                if (first_error.empty()) {
                    first_error = apply_error.empty()
                                      ? "unable to apply process pressure policy"
                                      : std::move(apply_error);
                }
            }
        }
    };

    // Reclaim hidden-tab working sets before touching visible tabs so the
    // foreground gets first claim on memory during elevated/critical pressure.
    apply_visibility(FrameVisibility::Hidden);
    apply_visibility(FrameVisibility::Visible);

    if (!all_applied) {
        *error = std::move(first_error);
    }
    return all_applied;
}

bool ZenithProcessTabController::contains(std::uint64_t session_id) const noexcept {
    return impl_->entries.find(session_id) != impl_->entries.end();
}

FramePressure ZenithProcessTabController::global_pressure() const noexcept {
    return impl_->pressure;
}

FramePressure ZenithProcessTabController::pressure_source(
    ZenithProcessPressureSource source) const noexcept {
    const std::size_t source_index = pressure_source_index(source);
    return source_index < impl_->pressure_sources.size()
        ? impl_->pressure_sources[source_index]
        : FramePressure::Normal;
}

ZenithProcessTabControllerStats ZenithProcessTabController::stats() const noexcept {
    return impl_->statistics;
}

ZenithTabActivitySink make_zenith_tab_runtime_activity_sink(
    ZenithTabRuntime* runtime) {
    return [runtime](
               FrameVisibility visibility,
               FramePressure pressure,
               std::int64_t scroll_velocity_q8_per_second,
               std::string* error) {
        if (runtime == nullptr || error == nullptr) {
            if (error != nullptr) {
                *error = "invalid zenith tab runtime activity sink";
            }
            return false;
        }
        return runtime->set_activity(
            visibility,
            pressure,
            scroll_velocity_q8_per_second,
            error);
    };
}

} // namespace zevryon::massivedoc
