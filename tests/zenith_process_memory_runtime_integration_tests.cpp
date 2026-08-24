#include "compact_document.hpp"
#include "layout_checkpoint.hpp"
#include "massivedoc_store.hpp"
#include "shared_source_prefetch_pool.hpp"
#include "zenith_process_memory_pressure.hpp"
#include "zenith_process_tab_controller.hpp"
#include "zenith_tab_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

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
                   "zevryon-process-memory-runtime-integration";
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
            700U,
            kRecordBytes,
            [&generated](std::span<std::byte> target) {
                for (std::size_t index = 0U; index < target.size(); ++index) {
                    const std::uint64_t absolute =
                        generated + static_cast<std::uint64_t>(index);
                    const unsigned char value =
                        (absolute % 80U) == 79U
                            ? static_cast<unsigned char>('\n')
                            : static_cast<unsigned char>('m');
                    target[index] = static_cast<std::byte>(value);
                }
                generated += static_cast<std::uint64_t>(target.size());
                return target.size();
            },
            &error),
        "memory-pressure fixture append failed");
    require(generated == kRecordBytes, "memory-pressure fixture record incomplete");

    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = kRecordBytes;
    metadata.logical_records = 1U;
    metadata.logical_nodes = 16U;
    metadata.style_runs = 4U;
    metadata.resource_references = 1U;
    metadata.largest_record_bytes = kRecordBytes;
    StoreStats store_stats;
    require(writer.finalize(metadata, &store_stats, &error),
            "memory-pressure fixture finalize failed");

    ArenaConfig arena_config;
    arena_config.records_per_block = 1U;
    arena_config.estimated_bytes_per_line = 96U;
    arena_config.line_height_q8 = 18U * 256U;
    arena_config.vertical_padding_q8 = 12U * 256U;
    ArenaStats arena_stats;
    require(
        build_compact_arena(fixture.root, arena_config, &arena_stats, &error),
        "memory-pressure compact arena build failed");

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
        "memory-pressure checkpoint build failed");
    return fixture;
}

void test_memory_pressure_drives_real_tab_runtimes(const Fixture& fixture) {
    SharedSourcePrefetchPool pool({1U, 128U * 1024U});
    ZenithTabRuntimeConfig config;
    config.layout = fixture.layout;
    config.frame_budget = FrameBudgetPolicy{
        1'000'000U,
        100'000U,
        10'000U,
        5'000U};
    config.prefetch_reserve_us = 100U;
    config.prefetch_bytes = 64U * 1024U;

    ZenithTabRuntime hidden(fixture.root, &pool, 1001U, config);
    ZenithTabRuntime visible(fixture.root, &pool, 1002U, config);
    std::string error;
    require(hidden.open(&error), "hidden runtime open failed");
    require(visible.open(&error), "visible runtime open failed");

    ZenithProcessTabController controller;
    require(
        controller.register_tab(
            1001U,
            FrameVisibility::Hidden,
            0,
            make_zenith_tab_runtime_activity_sink(&hidden),
            &error),
        "hidden runtime controller registration failed");
    require(
        controller.register_tab(
            1002U,
            FrameVisibility::Visible,
            4096,
            make_zenith_tab_runtime_activity_sink(&visible),
            &error),
        "visible runtime controller registration failed");

    require(pool.status().active_sessions == 1U,
            "hidden registration did not deactivate shared prefetch authority");
    const std::uint64_t background_before =
        hidden.hot_scroll_stats().background_trim_calls;
    const std::uint64_t critical_before =
        hidden.hot_scroll_stats().critical_trim_calls;

    ZenithProcessMemoryPressurePolicy policy;
    const ZenithProcessMemorySnapshot critical_snapshot{64U, 70U, 1000U};
    require(
        apply_zenith_process_memory_pressure_snapshot(
            &policy,
            &controller,
            critical_snapshot,
            &error),
        "critical memory snapshot application failed");
    require(controller.global_pressure() == FramePressure::Critical,
            "critical memory snapshot did not reach controller");
    require(hidden.pressure() == FramePressure::Critical &&
                hidden.visibility() == FrameVisibility::Hidden,
            "hidden runtime did not receive critical pressure");
    require(visible.pressure() == FramePressure::Critical &&
                visible.visibility() == FrameVisibility::Visible,
            "visible runtime did not receive critical pressure");
    require(
        hidden.hot_scroll_stats().critical_trim_calls == critical_before + 1U,
        "hidden runtime did not perform critical trim");
    require(pool.status().active_sessions == 1U,
            "critical pressure activated hidden speculative authority");

    const std::uint64_t visible_prefetch_before =
        visible.stats().prefetch_schedule_accepts;
    LayoutWindowResult visible_result;
    bool used_checkpoint = false;
    require(
        visible.layout(
            0U,
            800U * 256U,
            720U * 256U,
            0U,
            128U,
            &visible_result,
            &used_checkpoint,
            &error),
        "visible critical-pressure layout failed");
    require(used_checkpoint && !visible_result.fragments.empty(),
            "visible critical-pressure layout lost foreground rendering");
    require(
        visible.stats().prefetch_schedule_accepts == visible_prefetch_before,
        "critical visible layout admitted speculative prefetch");

    LayoutWindowResult hidden_result;
    used_checkpoint = true;
    require(
        hidden.layout(
            0U,
            800U * 256U,
            720U * 256U,
            0U,
            128U,
            &hidden_result,
            &used_checkpoint,
            &error),
        "hidden critical-pressure suppression call failed");
    require(hidden_result.fragments.empty() && !used_checkpoint,
            "hidden critical-pressure runtime performed foreground layout");

    const auto controller_before_repeat = controller.stats();
    const std::uint64_t trim_before_repeat =
        hidden.hot_scroll_stats().critical_trim_calls;
    require(
        apply_zenith_process_memory_pressure_snapshot(
            &policy,
            &controller,
            critical_snapshot,
            &error),
        "repeated critical snapshot failed");
    require(controller.stats().pressure_changes ==
                controller_before_repeat.pressure_changes,
            "repeated critical snapshot caused redundant controller pass");
    require(hidden.hot_scroll_stats().critical_trim_calls == trim_before_repeat,
            "repeated critical snapshot retrimmed hidden runtime");

    const ZenithProcessMemorySnapshot recovered_snapshot{64U, 200U, 1000U};
    require(
        apply_zenith_process_memory_pressure_snapshot(
            &policy,
            &controller,
            recovered_snapshot,
            &error),
        "normal memory recovery failed");
    require(controller.global_pressure() == FramePressure::Normal,
            "memory recovery did not restore normal pressure");
    require(hidden.hot_scroll_stats().background_trim_calls == background_before + 1U,
            "hidden runtime did not apply recovered background policy");
    require(policy.stats().samples == 3U && policy.stats().pressure_changes == 2U,
            "memory-pressure policy transition telemetry mismatch");
}

} // namespace

int main() {
    const Fixture fixture = build_fixture();
    test_memory_pressure_drives_real_tab_runtimes(fixture);

    std::error_code ignored;
    std::filesystem::remove_all(fixture.root, ignored);
    require(!ignored, "memory-pressure fixture cleanup failed");
    std::cout << "Zevryon process memory/runtime integration tests passed\n";
    return 0;
}
