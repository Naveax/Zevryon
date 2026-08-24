#include "compact_document.hpp"
#include "layout_checkpoint.hpp"
#include "massivedoc_store.hpp"
#include "zenith_process_runtime_services.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace zevryon::massivedoc;
using namespace std::chrono_literals;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool value, std::string_view message) {
    if (!value) {
        fail(message);
    }
}

struct Fixture {
    std::filesystem::path root;
    LayoutConfig layout;
};

Fixture build_fixture() {
    Fixture fixture;
    fixture.root = std::filesystem::temp_directory_path() /
                   "zevryon-dormant-tab-slots-tests";
    std::error_code ignored;
    std::filesystem::remove_all(fixture.root, ignored);

    constexpr std::uint64_t kRecordBytes = 512U * 1024U;
    StoreWriter writer(
        fixture.root,
        {.segment_bytes = 256U * 1024U, .records_per_search_block = 64U});
    std::string error;
    std::uint64_t generated = 0U;
    require(
        writer.append_stream(
            810U,
            kRecordBytes,
            [&generated](std::span<std::byte> target) {
                for (std::size_t index = 0U; index < target.size(); ++index) {
                    const std::uint64_t absolute =
                        generated + static_cast<std::uint64_t>(index);
                    const unsigned char value =
                        (absolute % 80U) == 79U
                            ? static_cast<unsigned char>('\n')
                            : static_cast<unsigned char>('d');
                    target[index] = static_cast<std::byte>(value);
                }
                generated += static_cast<std::uint64_t>(target.size());
                return target.size();
            },
            &error),
        "dormant slot fixture append failed");

    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = kRecordBytes;
    metadata.logical_records = 1U;
    metadata.logical_nodes = 16U;
    metadata.largest_record_bytes = kRecordBytes;
    StoreStats store_stats;
    require(writer.finalize(metadata, &store_stats, &error),
            "dormant slot fixture finalize failed");

    ArenaConfig arena_config;
    arena_config.records_per_block = 1U;
    arena_config.estimated_bytes_per_line = 96U;
    arena_config.line_height_q8 = 18U * 256U;
    arena_config.vertical_padding_q8 = 12U * 256U;
    ArenaStats arena_stats;
    require(
        build_compact_arena(
            fixture.root,
            arena_config,
            &arena_stats,
            &error),
        "dormant slot compact arena build failed");

    fixture.layout.checkpoint_stride_bytes = 16U * 1024U;
    fixture.layout.checkpoint_min_record_bytes = 1U;
    fixture.layout.max_checkpoint_cache_bytes = 256U * 1024U;
    fixture.layout.max_source_window_cache_bytes = 128U * 1024U;

    LayoutCheckpointConfig checkpoint;
    checkpoint.width_q8 = 800U * 256U;
    checkpoint.average_advance_q8 = fixture.layout.average_advance_q8;
    checkpoint.line_height_q8 = fixture.layout.line_height_q8;
    checkpoint.horizontal_padding_q8 = fixture.layout.horizontal_padding_q8;
    checkpoint.vertical_padding_q8 = fixture.layout.vertical_padding_q8;
    checkpoint.target_stride_bytes = fixture.layout.checkpoint_stride_bytes;
    LayoutCheckpointStats checkpoint_stats;
    require(
        build_layout_checkpoint(
            fixture.root,
            0U,
            checkpoint,
            &checkpoint_stats,
            &error),
        "dormant slot checkpoint build failed");
    return fixture;
}

ForegroundLayoutRequest layout_request(std::uint64_t request_id) {
    ForegroundLayoutRequest request;
    request.request_id = request_id;
    request.scroll_y_q8 = 0U;
    request.viewport_width_q8 = 800U * 256U;
    request.viewport_height_q8 = 720U * 256U;
    request.overscan_q8 = 0U;
    request.max_fragments = 128U;
    return request;
}

bool take_ready_for(
    ZenithProcessRuntimeServices* services,
    std::uint64_t session_id,
    ForegroundLayoutReady* ready,
    std::chrono::milliseconds timeout) {
    if (services == nullptr || ready == nullptr) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (services->try_take_tab_layout_async(session_id, ready)) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return services->try_take_tab_layout_async(session_id, ready);
}

void test_hidden_slots_materialize_only_on_demand(const Fixture& fixture) {
    const std::vector<ZenithProcessMemorySnapshot> snapshots{
        ZenithProcessMemorySnapshot{64U, 70U, 1000U},
        ZenithProcessMemorySnapshot{64U, 200U, 1000U},
    };
    std::size_t snapshot_index = 0U;

    ZenithProcessRuntimeServicesConfig config;
    config.prefetch_worker_count = 1U;
    config.prefetch_ready_bytes = 128U * 1024U;
    config.foreground_layout_worker_count = 1U;
    config.foreground_layout_ready_bytes = 512U * 1024U;
    config.foreground_layout_max_fragments = 256U;
    ZenithProcessRuntimeServices services(
        config,
        [&snapshots, &snapshot_index](
            ZenithProcessMemorySnapshot* snapshot,
            std::string* error) {
            if (snapshot == nullptr || error == nullptr ||
                snapshot_index >= snapshots.size()) {
                return false;
            }
            *snapshot = snapshots[snapshot_index++];
            error->clear();
            return true;
        });
    std::string error;

    require(
        services.open_tab(
            1001U,
            fixture.root,
            DeviceFrameProfile::Desktop,
            fixture.layout,
            FrameVisibility::Hidden,
            0,
            &error),
        "dormant hidden tab open failed");
    require(
        services.open_tab(
            1002U,
            fixture.root,
            DeviceFrameProfile::Desktop,
            fixture.layout,
            FrameVisibility::Visible,
            4096,
            &error),
        "visible tab open failed");

    auto status = services.status();
    require(status.tabs == 2U && status.materialized_tabs == 1U,
            "hidden tab was eagerly materialized");
    require(status.prefetch_pool.sessions == 1U &&
                status.prefetch_pool.active_sessions == 1U &&
                status.prefetch_pool.live_threads == 0U,
            "dormant hidden tab consumed source-prefetch resources");
    require(status.foreground_layout_pool.sessions == 1U &&
                status.foreground_layout_pool.active_sessions == 1U &&
                status.foreground_layout_pool.live_threads == 1U,
            "visible runtime did not bind one process foreground worker pool");
    require(!services.tab_materialized(1001U) && services.tab(1001U) == nullptr,
            "hidden dormant slot exposed a runtime");
    require(services.tab_materialized(1002U) && services.tab(1002U) != nullptr,
            "visible slot did not materialize runtime");
    require(
        services.request_tab_layout_async(1001U, layout_request(1U)) ==
            ForegroundLayoutWorkerScheduleResult::Inactive,
        "dormant hidden slot accepted foreground work");

    require(
        services.set_tab_activity(
            1002U,
            FrameVisibility::Hidden,
            0,
            &error),
        "visible-to-hidden transition failed");
    status = services.status();
    require(status.materialized_tabs == 1U &&
                status.prefetch_pool.sessions == 1U &&
                status.prefetch_pool.active_sessions == 0U &&
                status.foreground_layout_pool.sessions == 1U &&
                status.foreground_layout_pool.active_sessions == 0U,
            "normal hidden transition lost bounded retained runtime contract");

    require(
        services.set_tab_activity(
            1001U,
            FrameVisibility::Visible,
            4096,
            &error),
        "dormant-to-visible materialization failed");
    status = services.status();
    require(status.materialized_tabs == 2U &&
                status.prefetch_pool.sessions == 2U &&
                status.prefetch_pool.active_sessions == 1U &&
                status.foreground_layout_pool.sessions == 2U &&
                status.foreground_layout_pool.active_sessions == 1U,
            "visible materialization accounting mismatch");

    require(
        services.on_event_loop_tick(0U, nullptr, &error) ==
            ZenithProcessMemoryPollResult::Sampled,
        "critical process-memory sample failed");
    status = services.status();
    require(status.memory_pressure.pressure == FramePressure::Critical,
            "critical memory sample did not reach process owner");
    require(status.materialized_tabs == 1U &&
                status.prefetch_pool.sessions == 1U &&
                status.prefetch_pool.active_sessions == 1U &&
                status.foreground_layout_pool.sessions == 1U &&
                status.foreground_layout_pool.active_sessions == 1U,
            "critical pressure did not dematerialize hidden runtime");
    require(!services.tab_materialized(1002U) && services.tab(1002U) == nullptr,
            "critical hidden runtime survived as materialized state");
    require(services.tab_materialized(1001U),
            "critical pressure dematerialized visible runtime");

    ZenithTabRuntime* visible = services.tab(1001U);
    const std::uint64_t prefetch_before =
        visible->stats().prefetch_schedule_accepts;
    require(
        services.request_tab_layout_async(1001U, layout_request(1U)) ==
            ForegroundLayoutWorkerScheduleResult::Accepted,
        "critical visible async layout request failed");
    ForegroundLayoutReady ready;
    require(take_ready_for(&services, 1001U, &ready, 5s),
            "critical visible async layout did not complete");
    require(ready.succeeded && ready.used_checkpoint_path &&
                !ready.result.fragments.empty(),
            "critical visible async layout lost foreground output");
    require(visible->stats().prefetch_schedule_accepts == prefetch_before,
            "critical visible slot scheduled speculative prefetch");

    require(
        services.on_event_loop_tick(99U, nullptr, &error) ==
            ZenithProcessMemoryPollResult::Throttled,
        "critical cadence did not throttle early process tick");
    require(snapshot_index == 1U,
            "throttled critical tick touched snapshot provider");

    require(
        services.on_event_loop_tick(100U, nullptr, &error) ==
            ZenithProcessMemoryPollResult::Sampled,
        "normal recovery sample failed");
    require(services.status().memory_pressure.pressure == FramePressure::Normal,
            "normal recovery did not restore process pressure");
    require(!services.tab_materialized(1002U),
            "normal recovery eagerly rematerialized hidden tab");

    require(
        services.set_tab_activity(
            1002U,
            FrameVisibility::Visible,
            -4096,
            &error),
        "post-critical hidden tab did not rematerialize on visibility");
    status = services.status();
    require(status.materialized_tabs == 2U &&
                status.prefetch_pool.sessions == 2U &&
                status.prefetch_pool.active_sessions == 2U &&
                status.foreground_layout_pool.sessions == 2U &&
                status.foreground_layout_pool.active_sessions == 2U,
            "post-critical rematerialization accounting mismatch");

    require(services.close_tab(1001U) && services.close_tab(1002U),
            "dormant slot close failed");
    status = services.status();
    require(status.tabs == 0U && status.materialized_tabs == 0U &&
                status.prefetch_pool.sessions == 0U &&
                status.foreground_layout_pool.sessions == 0U,
            "closed dormant slots retained process resources");
}

} // namespace

int main() {
    const Fixture fixture = build_fixture();
    test_hidden_slots_materialize_only_on_demand(fixture);

    std::error_code ignored;
    std::filesystem::remove_all(fixture.root, ignored);
    require(!ignored, "dormant slot fixture cleanup failed");
    std::cout << "Zevryon dormant tab slots tests passed\n";
    return 0;
}
