#include "compact_document.hpp"
#include "layout_checkpoint.hpp"
#include "massivedoc_store.hpp"
#include "zenith_process_runtime_services.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace zevryon::massivedoc;

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
                   "zevryon-process-runtime-services-tests";
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
            800U,
            kRecordBytes,
            [&generated](std::span<std::byte> target) {
                for (std::size_t index = 0U; index < target.size(); ++index) {
                    const std::uint64_t absolute =
                        generated + static_cast<std::uint64_t>(index);
                    const unsigned char value =
                        (absolute % 80U) == 79U
                            ? static_cast<unsigned char>('\n')
                            : static_cast<unsigned char>('r');
                    target[index] = static_cast<std::byte>(value);
                }
                generated += static_cast<std::uint64_t>(target.size());
                return target.size();
            },
            &error),
        "process runtime fixture append failed");
    require(generated == kRecordBytes,
            "process runtime fixture record incomplete");

    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = kRecordBytes;
    metadata.logical_records = 1U;
    metadata.logical_nodes = 16U;
    metadata.style_runs = 4U;
    metadata.resource_references = 1U;
    metadata.largest_record_bytes = kRecordBytes;
    StoreStats store_stats;
    require(writer.finalize(metadata, &store_stats, &error),
            "process runtime fixture finalize failed");

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
        "process runtime compact arena build failed");

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
        "process runtime checkpoint build failed");
    return fixture;
}

void test_process_owned_services(const Fixture& fixture) {
    const std::vector<ZenithProcessMemorySnapshot> snapshots{
        ZenithProcessMemorySnapshot{64U, 70U, 1000U},
        ZenithProcessMemorySnapshot{64U, 200U, 1000U},
    };
    std::size_t snapshot_index = 0U;

    ZenithProcessRuntimeServicesConfig config;
    config.prefetch_worker_count = 1U;
    config.prefetch_ready_bytes = 128U * 1024U;
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
    require(services.valid(), "process runtime services are invalid");

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
        "hidden process-owned tab open failed");
    require(
        services.open_tab(
            1002U,
            fixture.root,
            DeviceFrameProfile::Desktop,
            fixture.layout,
            FrameVisibility::Visible,
            4096,
            &error),
        "visible process-owned tab open failed");
    require(
        !services.open_tab(
            1002U,
            fixture.root,
            DeviceFrameProfile::Desktop,
            fixture.layout,
            FrameVisibility::Visible,
            4096,
            &error),
        "duplicate process-owned tab identity was accepted");

    ZenithProcessRuntimeServicesStatus opened = services.status();
    require(opened.tabs == 2U,
            "process-owned tab registry count mismatch");
    require(opened.prefetch_pool.sessions == 2U &&
                opened.prefetch_pool.active_sessions == 1U,
            "process-owned shared pool session accounting mismatch");
    require(opened.prefetch_pool.live_threads == 0U,
            "opening tabs eagerly started shared prefetch workers");
    require(opened.tab_controller.visible_tabs == 1U &&
                opened.tab_controller.hidden_tabs == 1U,
            "process-owned controller visibility accounting mismatch");

    ZenithTabRuntime* hidden = services.tab(1001U);
    ZenithTabRuntime* visible = services.tab(1002U);
    require(hidden != nullptr && visible != nullptr,
            "process-owned runtime lookup failed");

    const std::uint64_t hidden_critical_before =
        hidden->hot_scroll_stats().critical_trim_calls;
    require(
        services.on_event_loop_tick(0U, nullptr, &error) ==
            ZenithProcessMemoryPollResult::Sampled,
        "process event-loop critical sample failed");
    require(services.status().memory_pressure.pressure ==
                FramePressure::Critical,
            "process event-loop sample did not enter critical pressure");
    require(hidden->hot_scroll_stats().critical_trim_calls ==
                hidden_critical_before + 1U,
            "process owner did not critically trim hidden runtime");
    require(visible->pressure() == FramePressure::Critical,
            "process owner did not propagate critical pressure to visible runtime");

    const std::uint64_t visible_prefetch_before =
        visible->stats().prefetch_schedule_accepts;
    LayoutWindowResult result;
    bool used_checkpoint = false;
    require(
        visible->layout(
            0U,
            800U * 256U,
            720U * 256U,
            0U,
            128U,
            &result,
            &used_checkpoint,
            &error),
        "visible process-owned critical layout failed");
    require(used_checkpoint && !result.fragments.empty(),
            "visible process-owned runtime lost foreground rendering");
    require(visible->stats().prefetch_schedule_accepts ==
                visible_prefetch_before,
            "critical process-owned visible runtime scheduled speculative work");

    require(
        services.on_event_loop_tick(99U, nullptr, &error) ==
            ZenithProcessMemoryPollResult::Throttled,
        "process event-loop cadence failed to throttle critical tick");
    require(snapshot_index == 1U,
            "throttled process event-loop tick touched snapshot provider");

    const std::uint64_t background_before =
        hidden->hot_scroll_stats().background_trim_calls;
    require(
        services.on_event_loop_tick(100U, nullptr, &error) ==
            ZenithProcessMemoryPollResult::Sampled,
        "process event-loop recovery sample failed");
    require(services.status().memory_pressure.pressure ==
                FramePressure::Normal,
            "process event-loop recovery did not restore normal pressure");
    require(hidden->hot_scroll_stats().background_trim_calls ==
                background_before + 1U,
            "process owner did not restore hidden background policy");

    require(services.close_tab(1001U),
            "process owner failed to close hidden tab");
    require(services.close_tab(1002U),
            "process owner failed to close visible tab");
    const ZenithProcessRuntimeServicesStatus closed = services.status();
    require(closed.tabs == 0U && closed.prefetch_pool.sessions == 0U &&
                closed.tab_controller.registered_tabs == 0U,
            "process owner retained closed tab state");

    require(
        services.open_tab(
            1001U,
            fixture.root,
            DeviceFrameProfile::Desktop,
            fixture.layout,
            FrameVisibility::Hidden,
            0,
            &error),
        "closed process tab identity was not reusable");
    require(services.close_tab(1001U),
            "reopened process tab did not close cleanly");
}

void test_invalid_process_owner_config() {
    ZenithProcessRuntimeServicesConfig config;
    config.prefetch_worker_count = 65U;
    require(!config.valid(),
            "process owner accepted unbounded worker count");

    config = {};
    config.prefetch_ready_bytes = 0U;
    require(!config.valid(),
            "process owner accepted zero ready-result budget");
}

} // namespace

int main() {
    const Fixture fixture = build_fixture();
    test_process_owned_services(fixture);
    test_invalid_process_owner_config();

    std::error_code ignored;
    std::filesystem::remove_all(fixture.root, ignored);
    require(!ignored, "process runtime fixture cleanup failed");
    std::cout << "Zevryon process runtime services tests passed\n";
    return 0;
}
