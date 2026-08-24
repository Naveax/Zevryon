#pragma once

#include "device_frame_profile.hpp"
#include "foreground_layout_worker_pool.hpp"
#include "shared_record_length_authority.hpp"
#include "shared_source_prefetch_pool.hpp"
#include "zenith_process_memory_sampler.hpp"
#include "zenith_process_tab_controller.hpp"
#include "zenith_tab_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace zevryon::massivedoc {

struct ZenithProcessRuntimeServicesConfig {
    std::size_t prefetch_worker_count{2U};
    std::size_t prefetch_ready_bytes{2U * 1024U * 1024U};
    std::size_t foreground_layout_worker_count{2U};
    std::size_t foreground_layout_ready_bytes{4U * 1024U * 1024U};
    std::size_t foreground_layout_max_fragments{4096U};
    SharedRecordLengthAuthorityConfig record_length{};
    ZenithProcessMemoryPressureConfig memory_pressure{};
    ZenithProcessMemorySamplerConfig memory_sampler{};

    bool valid() const noexcept;
};

struct ZenithProcessRuntimeServicesStatus {
    std::size_t tabs{0U};
    std::size_t materialized_tabs{0U};
    std::size_t retired_runtime_generations{0U};
    SharedSourcePrefetchPoolStatus prefetch_pool{};
    ForegroundLayoutWorkerPoolStatus foreground_layout_pool{};
    SharedRecordLengthAuthorityStatus record_lengths{};
    ZenithProcessTabControllerStats tab_controller{};
    ZenithProcessMemoryPressureStats memory_pressure{};
    ZenithProcessMemorySamplerStats memory_sampler{};
};

class ZenithProcessRuntimeServices final {
public:
    explicit ZenithProcessRuntimeServices(
        ZenithProcessRuntimeServicesConfig config = {},
        ZenithProcessMemorySnapshotProvider snapshot_provider = {});
    ~ZenithProcessRuntimeServices();

    ZenithProcessRuntimeServices(const ZenithProcessRuntimeServices&) = delete;
    ZenithProcessRuntimeServices& operator=(const ZenithProcessRuntimeServices&) = delete;
    ZenithProcessRuntimeServices(ZenithProcessRuntimeServices&&) = delete;
    ZenithProcessRuntimeServices& operator=(ZenithProcessRuntimeServices&&) = delete;

    bool valid() const noexcept;

    bool open_tab(
        std::uint64_t session_id,
        const std::filesystem::path& store_root,
        DeviceFrameProfile profile,
        LayoutConfig layout,
        FrameVisibility visibility,
        std::int64_t scroll_velocity_q8_per_second,
        std::string* error);
    bool close_tab(std::uint64_t session_id) noexcept;
    bool set_tab_activity(
        std::uint64_t session_id,
        FrameVisibility visibility,
        std::int64_t scroll_velocity_q8_per_second,
        std::string* error);

    ForegroundLayoutWorkerScheduleResult request_tab_layout_async(
        std::uint64_t session_id,
        ForegroundLayoutRequest request) noexcept;
    bool try_take_tab_layout_async(
        std::uint64_t session_id,
        ForegroundLayoutReady* ready) noexcept;

    ZenithProcessMemoryPollResult on_event_loop_tick(
        std::uint64_t monotonic_ms,
        ZenithProcessMemorySnapshot* captured,
        std::string* error);
    ZenithProcessMemoryPollResult on_event_loop_tick_now(
        ZenithProcessMemorySnapshot* captured,
        std::string* error);

    bool tab_materialized(std::uint64_t session_id) const noexcept;
    ZenithTabRuntime* tab(std::uint64_t session_id) noexcept;
    const ZenithTabRuntime* tab(std::uint64_t session_id) const noexcept;
    ZenithProcessRuntimeServicesStatus status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zevryon::massivedoc
