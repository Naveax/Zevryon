#include "zenith_process_runtime_services.hpp"

#include "zenith_tab_runtime_profile.hpp"

#include <exception>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zevryon::massivedoc {
namespace {

SharedSourcePrefetchPoolConfig make_pool_config(
    const ZenithProcessRuntimeServicesConfig& config,
    SharedRecordLengthAuthority* authority) {
    SharedSourcePrefetchPoolConfig pool;
    pool.worker_count = config.prefetch_worker_count;
    pool.max_ready_bytes = config.prefetch_ready_bytes;
    pool.record_length_authority = authority;
    return pool;
}

std::int64_t normalized_velocity(
    FrameVisibility visibility,
    std::int64_t velocity) noexcept {
    return visibility == FrameVisibility::Visible ? velocity : 0;
}

} // namespace

bool ZenithProcessRuntimeServicesConfig::valid() const noexcept {
    return prefetch_worker_count > 0U &&
           prefetch_worker_count <= 64U &&
           prefetch_ready_bytes > 0U &&
           record_length.valid() &&
           memory_pressure.valid() &&
           memory_sampler.valid();
}

struct ZenithProcessRuntimeServices::Impl {
    struct TabSlot {
        std::filesystem::path store_root;
        DeviceFrameProfile profile{DeviceFrameProfile::MidPhone};
        LayoutConfig layout{};
        FrameVisibility visibility{FrameVisibility::Hidden};
        std::int64_t velocity{0};
        std::unique_ptr<ZenithTabRuntime> runtime;
        bool controller_registered{false};
    };

    Impl(
        ZenithProcessRuntimeServicesConfig config_value,
        ZenithProcessMemorySnapshotProvider provider)
        : config(config_value),
          record_lengths(config_value.record_length),
          prefetch_pool(make_pool_config(config_value, &record_lengths)),
          memory_pressure(config_value.memory_pressure),
          memory_sampler(config_value.memory_sampler, std::move(provider)) {}

    ZenithTabActivitySink sink_for(std::uint64_t session_id) {
        return [this, session_id](
                   FrameVisibility visibility,
                   FramePressure pressure,
                   std::int64_t velocity,
                   std::string* error) {
            return apply_slot(
                session_id,
                visibility,
                pressure,
                velocity,
                error);
        };
    }

    bool apply_slot(
        std::uint64_t session_id,
        FrameVisibility visibility,
        FramePressure pressure,
        std::int64_t velocity,
        std::string* error) {
        if (error == nullptr) {
            return false;
        }
        error->clear();
        const auto found = tabs.find(session_id);
        if (found == tabs.end()) {
            *error = "unknown process runtime tab slot";
            return false;
        }
        TabSlot& slot = found->second;
        const std::int64_t target_velocity =
            normalized_velocity(visibility, velocity);

        if (visibility == FrameVisibility::Hidden && !slot.runtime) {
            slot.visibility = visibility;
            slot.velocity = 0;
            return true;
        }

        if (!slot.runtime) {
            ZenithTabRuntimeConfig tab_config =
                make_zenith_tab_runtime_config(
                    slot.profile,
                    slot.layout,
                    &record_lengths);
            auto runtime = std::make_unique<ZenithTabRuntime>(
                slot.store_root,
                &prefetch_pool,
                session_id,
                tab_config);
            if (!runtime->open(error)) {
                return false;
            }
            if (!runtime->set_activity(
                    visibility,
                    pressure,
                    target_velocity,
                    error)) {
                return false;
            }
            slot.runtime = std::move(runtime);
            ++materialized_tabs;
        } else if (!slot.runtime->set_activity(
                       visibility,
                       pressure,
                       target_velocity,
                       error)) {
            return false;
        }

        slot.visibility = visibility;
        slot.velocity = target_velocity;

        if (visibility == FrameVisibility::Hidden &&
            pressure == FramePressure::Critical &&
            slot.runtime) {
            slot.runtime.reset();
            if (materialized_tabs > 0U) {
                --materialized_tabs;
            }
            pending_controller_unregistrations.push_back(session_id);
        }
        return true;
    }

    bool register_controller(
        std::uint64_t session_id,
        FrameVisibility visibility,
        std::int64_t velocity,
        std::string* error) {
        const auto found = tabs.find(session_id);
        if (found == tabs.end() || error == nullptr) {
            return false;
        }
        TabSlot& slot = found->second;
        if (slot.controller_registered) {
            return true;
        }
        if (!tab_controller.register_tab(
                session_id,
                visibility,
                velocity,
                sink_for(session_id),
                error)) {
            return false;
        }
        slot.controller_registered = true;
        return true;
    }

    void drain_pending_controller_unregistrations() noexcept {
        for (const std::uint64_t session_id :
             pending_controller_unregistrations) {
            const auto found = tabs.find(session_id);
            if (found == tabs.end() ||
                !found->second.controller_registered ||
                found->second.runtime) {
                continue;
            }
            if (tab_controller.unregister_tab(session_id)) {
                found->second.controller_registered = false;
            }
        }
        pending_controller_unregistrations.clear();
    }

    ZenithProcessRuntimeServicesConfig config;
    SharedRecordLengthAuthority record_lengths;
    SharedSourcePrefetchPool prefetch_pool;
    ZenithProcessTabController tab_controller;
    ZenithProcessMemoryPressurePolicy memory_pressure;
    ZenithProcessMemorySampler memory_sampler;
    std::unordered_map<std::uint64_t, TabSlot> tabs;
    std::size_t materialized_tabs{0U};
    std::vector<std::uint64_t> pending_controller_unregistrations;
};

ZenithProcessRuntimeServices::ZenithProcessRuntimeServices(
    ZenithProcessRuntimeServicesConfig config,
    ZenithProcessMemorySnapshotProvider snapshot_provider)
    : impl_(std::make_unique<Impl>(
          config,
          std::move(snapshot_provider))) {}

ZenithProcessRuntimeServices::~ZenithProcessRuntimeServices() = default;

bool ZenithProcessRuntimeServices::valid() const noexcept {
    return impl_->config.valid() &&
           impl_->record_lengths.valid() &&
           impl_->prefetch_pool.valid() &&
           impl_->memory_pressure.valid() &&
           impl_->memory_sampler.valid();
}

bool ZenithProcessRuntimeServices::open_tab(
    std::uint64_t session_id,
    const std::filesystem::path& store_root,
    DeviceFrameProfile profile,
    LayoutConfig layout,
    FrameVisibility visibility,
    std::int64_t scroll_velocity_q8_per_second,
    std::string* error) {
    if (error == nullptr || !valid()) {
        if (error != nullptr) {
            *error = "invalid process runtime tab open";
        }
        return false;
    }
    error->clear();
    if (impl_->tabs.find(session_id) != impl_->tabs.end()) {
        *error = "process runtime tab session is already open";
        return false;
    }

    try {
        Impl::TabSlot slot;
        slot.store_root = store_root;
        slot.profile = profile;
        slot.layout = layout;
        slot.visibility = visibility;
        slot.velocity = normalized_velocity(
            visibility,
            scroll_velocity_q8_per_second);
        const auto inserted =
            impl_->tabs.emplace(session_id, std::move(slot));
        if (!inserted.second) {
            *error = "unable to register process runtime tab slot";
            return false;
        }

        if (visibility == FrameVisibility::Hidden) {
            return true;
        }

        std::string controller_error;
        if (!impl_->register_controller(
                session_id,
                visibility,
                scroll_velocity_q8_per_second,
                &controller_error)) {
            impl_->tabs.erase(inserted.first);
            *error = controller_error.empty()
                         ? "unable to register visible tab with process controller"
                         : std::move(controller_error);
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        *error = std::string("unable to allocate process runtime tab: ") +
                 exception.what();
        return false;
    } catch (...) {
        *error = "unable to allocate process runtime tab";
        return false;
    }
}

bool ZenithProcessRuntimeServices::close_tab(
    std::uint64_t session_id) noexcept {
    const auto found = impl_->tabs.find(session_id);
    if (found == impl_->tabs.end()) {
        return false;
    }
    if (found->second.controller_registered &&
        !impl_->tab_controller.unregister_tab(session_id)) {
        return false;
    }
    if (found->second.runtime && impl_->materialized_tabs > 0U) {
        --impl_->materialized_tabs;
    }
    impl_->tabs.erase(found);
    return true;
}

bool ZenithProcessRuntimeServices::set_tab_activity(
    std::uint64_t session_id,
    FrameVisibility visibility,
    std::int64_t scroll_velocity_q8_per_second,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    const auto found = impl_->tabs.find(session_id);
    if (found == impl_->tabs.end()) {
        *error = "unknown process runtime tab";
        return false;
    }
    Impl::TabSlot& slot = found->second;

    if (!slot.controller_registered) {
        if (visibility == FrameVisibility::Hidden) {
            slot.visibility = FrameVisibility::Hidden;
            slot.velocity = 0;
            return true;
        }
        return impl_->register_controller(
            session_id,
            FrameVisibility::Visible,
            scroll_velocity_q8_per_second,
            error);
    }

    const FrameVisibility previous_visibility = slot.visibility;
    const std::int64_t previous_velocity = slot.velocity;

    if (impl_->tab_controller.set_tab_activity(
            session_id,
            visibility,
            scroll_velocity_q8_per_second,
            error)) {
        impl_->drain_pending_controller_unregistrations();
        return true;
    }

    const std::string original_error = *error;
    static_cast<void>(impl_->tab_controller.unregister_tab(session_id));
    slot.controller_registered = false;
    std::string rollback_error;
    if (!impl_->register_controller(
            session_id,
            previous_visibility,
            previous_velocity,
            &rollback_error)) {
        *error = original_error +
                 "; process controller rollback failed: " +
                 rollback_error;
        return false;
    }
    *error = original_error;
    return false;
}

ZenithProcessMemoryPollResult ZenithProcessRuntimeServices::on_event_loop_tick(
    std::uint64_t monotonic_ms,
    ZenithProcessMemorySnapshot* captured,
    std::string* error) {
    const ZenithProcessMemoryPollResult result = impl_->memory_sampler.poll(
        monotonic_ms,
        &impl_->memory_pressure,
        &impl_->tab_controller,
        captured,
        error);
    impl_->drain_pending_controller_unregistrations();
    return result;
}

ZenithProcessMemoryPollResult ZenithProcessRuntimeServices::on_event_loop_tick_now(
    ZenithProcessMemorySnapshot* captured,
    std::string* error) {
    const ZenithProcessMemoryPollResult result = impl_->memory_sampler.poll_now(
        &impl_->memory_pressure,
        &impl_->tab_controller,
        captured,
        error);
    impl_->drain_pending_controller_unregistrations();
    return result;
}

bool ZenithProcessRuntimeServices::tab_materialized(
    std::uint64_t session_id) const noexcept {
    const auto found = impl_->tabs.find(session_id);
    return found != impl_->tabs.end() &&
           static_cast<bool>(found->second.runtime);
}

ZenithTabRuntime* ZenithProcessRuntimeServices::tab(
    std::uint64_t session_id) noexcept {
    const auto found = impl_->tabs.find(session_id);
    return found == impl_->tabs.end() ? nullptr : found->second.runtime.get();
}

const ZenithTabRuntime* ZenithProcessRuntimeServices::tab(
    std::uint64_t session_id) const noexcept {
    const auto found = impl_->tabs.find(session_id);
    return found == impl_->tabs.end() ? nullptr : found->second.runtime.get();
}

ZenithProcessRuntimeServicesStatus
ZenithProcessRuntimeServices::status() const {
    ZenithProcessRuntimeServicesStatus snapshot;
    snapshot.tabs = impl_->tabs.size();
    snapshot.materialized_tabs = impl_->materialized_tabs;
    snapshot.prefetch_pool = impl_->prefetch_pool.status();
    snapshot.record_lengths = impl_->record_lengths.status();
    snapshot.tab_controller = impl_->tab_controller.stats();
    snapshot.memory_pressure = impl_->memory_pressure.stats();
    snapshot.memory_sampler = impl_->memory_sampler.stats();
    return snapshot;
}

} // namespace zevryon::massivedoc
