#include "zenith_process_runtime_services.hpp"

#include "zenith_tab_runtime_profile.hpp"

#include <exception>
#include <unordered_map>
#include <utility>

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
    Impl(
        ZenithProcessRuntimeServicesConfig config_value,
        ZenithProcessMemorySnapshotProvider provider)
        : config(config_value),
          record_lengths(config_value.record_length),
          prefetch_pool(make_pool_config(config_value, &record_lengths)),
          memory_pressure(config_value.memory_pressure),
          memory_sampler(config_value.memory_sampler, std::move(provider)) {}

    ZenithProcessRuntimeServicesConfig config;
    SharedRecordLengthAuthority record_lengths;
    SharedSourcePrefetchPool prefetch_pool;
    ZenithProcessTabController tab_controller;
    ZenithProcessMemoryPressurePolicy memory_pressure;
    ZenithProcessMemorySampler memory_sampler;
    std::unordered_map<std::uint64_t, std::unique_ptr<ZenithTabRuntime>> tabs;
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
        ZenithTabRuntimeConfig tab_config = make_zenith_tab_runtime_config(
            profile,
            layout,
            &impl_->record_lengths);
        auto runtime = std::make_unique<ZenithTabRuntime>(
            store_root,
            &impl_->prefetch_pool,
            session_id,
            tab_config);
        if (!runtime->open(error)) {
            return false;
        }

        const auto inserted = impl_->tabs.emplace(session_id, std::move(runtime));
        if (!inserted.second) {
            *error = "unable to register process runtime tab";
            return false;
        }
        ZenithTabRuntime* const runtime_ptr = inserted.first->second.get();

        std::string controller_error;
        if (!impl_->tab_controller.register_tab(
                session_id,
                visibility,
                scroll_velocity_q8_per_second,
                make_zenith_tab_runtime_activity_sink(runtime_ptr),
                &controller_error)) {
            impl_->tabs.erase(inserted.first);
            *error = controller_error.empty()
                         ? "unable to register tab with process controller"
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
    if (!impl_->tab_controller.unregister_tab(session_id)) {
        return false;
    }
    impl_->tabs.erase(found);
    return true;
}

bool ZenithProcessRuntimeServices::set_tab_activity(
    std::uint64_t session_id,
    FrameVisibility visibility,
    std::int64_t scroll_velocity_q8_per_second,
    std::string* error) {
    return impl_->tab_controller.set_tab_activity(
        session_id,
        visibility,
        scroll_velocity_q8_per_second,
        error);
}

ZenithProcessMemoryPollResult ZenithProcessRuntimeServices::on_event_loop_tick(
    std::uint64_t monotonic_ms,
    ZenithProcessMemorySnapshot* captured,
    std::string* error) {
    return impl_->memory_sampler.poll(
        monotonic_ms,
        &impl_->memory_pressure,
        &impl_->tab_controller,
        captured,
        error);
}

ZenithProcessMemoryPollResult ZenithProcessRuntimeServices::on_event_loop_tick_now(
    ZenithProcessMemorySnapshot* captured,
    std::string* error) {
    return impl_->memory_sampler.poll_now(
        &impl_->memory_pressure,
        &impl_->tab_controller,
        captured,
        error);
}

ZenithTabRuntime* ZenithProcessRuntimeServices::tab(
    std::uint64_t session_id) noexcept {
    const auto found = impl_->tabs.find(session_id);
    return found == impl_->tabs.end() ? nullptr : found->second.get();
}

const ZenithTabRuntime* ZenithProcessRuntimeServices::tab(
    std::uint64_t session_id) const noexcept {
    const auto found = impl_->tabs.find(session_id);
    return found == impl_->tabs.end() ? nullptr : found->second.get();
}

ZenithProcessRuntimeServicesStatus
ZenithProcessRuntimeServices::status() const {
    ZenithProcessRuntimeServicesStatus snapshot;
    snapshot.tabs = impl_->tabs.size();
    snapshot.prefetch_pool = impl_->prefetch_pool.status();
    snapshot.record_lengths = impl_->record_lengths.status();
    snapshot.tab_controller = impl_->tab_controller.stats();
    snapshot.memory_pressure = impl_->memory_pressure.stats();
    snapshot.memory_sampler = impl_->memory_sampler.stats();
    return snapshot;
}

} // namespace zevryon::massivedoc
