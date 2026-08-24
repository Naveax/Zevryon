#include "compact_document.hpp"
#include "foreground_layout_worker_pool.hpp"
#include "layout_checkpoint.hpp"
#include "massivedoc_store.hpp"
#include "zenith_tab_runtime.hpp"

#include <chrono>
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
using namespace std::chrono_literals;

[[noreturn]] void die(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool value, std::string_view message) {
    if (!value) {
        die(message);
    }
}

struct Fixture {
    std::filesystem::path root;
    LayoutConfig layout;
};

Fixture build_fixture() {
    Fixture fixture;
    fixture.root =
        std::filesystem::temp_directory_path() / "zevryon-tab-async-layout-tests";
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
            1701U,
            kRecordBytes,
            [&generated](std::span<std::byte> target) {
                for (std::size_t index = 0U; index < target.size(); ++index) {
                    const std::uint64_t absolute =
                        generated + static_cast<std::uint64_t>(index);
                    const unsigned char value =
                        (absolute % 80U) == 79U
                            ? static_cast<unsigned char>('\n')
                            : static_cast<unsigned char>('x');
                    target[index] = static_cast<std::byte>(value);
                }
                generated += static_cast<std::uint64_t>(target.size());
                return target.size();
            },
            &error),
        "async fixture append failed");
    require(generated == kRecordBytes, "async fixture record incomplete");

    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = kRecordBytes;
    metadata.logical_records = 1U;
    metadata.logical_nodes = 16U;
    metadata.style_runs = 4U;
    metadata.resource_references = 1U;
    metadata.largest_record_bytes = kRecordBytes;
    StoreStats store_stats;
    require(writer.finalize(metadata, &store_stats, &error),
            "async fixture finalize failed");

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
        "async compact arena build failed");

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
        "async checkpoint build failed");
    return fixture;
}

ForegroundLayoutRequest request(std::uint64_t id, std::uint64_t scroll_y_q8 = 0U) {
    ForegroundLayoutRequest result;
    result.request_id = id;
    result.scroll_y_q8 = scroll_y_q8;
    result.viewport_width_q8 = 800U * 256U;
    result.viewport_height_q8 = 720U * 256U;
    result.overscan_q8 = 360U * 256U;
    result.max_fragments = 128U;
    return result;
}

ZenithTabRuntimeConfig runtime_config(const Fixture& fixture) {
    ZenithTabRuntimeConfig config;
    config.layout = fixture.layout;
    config.frame_budget = FrameBudgetPolicy{
        1'000'000U,
        100'000U,
        10'000U,
        5'000U};
    config.prefetch_reserve_us = 100U;
    config.prefetch_bytes = kIoWindowBytes;
    return config;
}

void test_async_request_worker_delivery_and_sync_fence(const Fixture& fixture) {
    SharedForegroundLayoutWorkerPool foreground_pool(
        {1U, {512U * 1024U, 256U}});
    ZenithTabRuntime runtime(
        fixture.root,
        nullptr,
        &foreground_pool,
        701U,
        runtime_config(fixture));

    std::string error;
    require(runtime.open(&error), "async runtime open failed");
    require(runtime.async_layout_enabled(), "async runtime did not enable worker path");
    require(foreground_pool.status().live_threads == 1U,
            "async runtime did not share configured foreground worker");
    require(
        runtime.set_activity(
            FrameVisibility::Visible,
            FramePressure::Normal,
            4096,
            &error),
        "async visible activity failed");

    LayoutWindowResult synchronous;
    bool used_checkpoint = true;
    require(
        !runtime.layout(
            0U,
            800U * 256U,
            720U * 256U,
            0U,
            128U,
            &synchronous,
            &used_checkpoint,
            &error),
        "synchronous layout remained enabled beside async worker path");
    require(
        error.find("synchronous layout is disabled") != std::string::npos,
        "synchronous async-mode rejection reason changed");

    require(
        runtime.request_layout_async(request(1U)) ==
            ForegroundLayoutWorkerScheduleResult::Accepted,
        "first async viewport request not accepted");
    require(foreground_pool.wait_idle_for(5s),
            "async foreground worker did not become idle");

    ForegroundLayoutReady ready;
    require(runtime.try_take_layout_async(&ready),
            "async ready viewport was not delivered");
    require(ready.request_id == 1U && ready.succeeded,
            "async ready viewport identity or success changed");
    require(ready.used_checkpoint_path && !ready.result.fragments.empty(),
            "async worker did not produce accelerated foreground fragments");
    require(!runtime.try_take_layout_async(&ready),
            "async viewport result was delivered more than once");

    const ZenithTabRuntimeStats stats = runtime.stats();
    require(stats.async_layout_requests == 1U,
            "async request accounting mismatch");
    require(stats.async_layout_accepts == 1U,
            "async acceptance accounting mismatch");
    require(stats.async_layout_ready_drains == 1U &&
                stats.async_layout_success_drains == 1U,
            "async ready accounting mismatch");
    require(runtime.hot_scroll_stats().layout_calls == 1U,
            "async request did not execute exactly one hot-scroll layout");
}

void test_hidden_authority_invalidates_ready_and_resumes_fresh(const Fixture& fixture) {
    SharedForegroundLayoutWorkerPool foreground_pool(
        {1U, {512U * 1024U, 256U}});
    ZenithTabRuntime runtime(
        fixture.root,
        nullptr,
        &foreground_pool,
        702U,
        runtime_config(fixture));

    std::string error;
    require(runtime.open(&error), "hidden-authority runtime open failed");
    require(
        runtime.set_activity(
            FrameVisibility::Visible,
            FramePressure::Normal,
            4096,
            &error),
        "hidden-authority visible activity failed");
    require(
        runtime.request_layout_async(request(10U)) ==
            ForegroundLayoutWorkerScheduleResult::Accepted,
        "hidden-authority baseline request not accepted");
    require(foreground_pool.wait_idle_for(5s),
            "hidden-authority baseline worker did not settle");
    require(foreground_pool.status().handoff.ready_results == 1U,
            "hidden-authority baseline ready result missing");

    require(
        runtime.set_activity(
            FrameVisibility::Hidden,
            FramePressure::Critical,
            0,
            &error),
        "critical hidden transition failed");
    require(foreground_pool.status().handoff.ready_results == 0U,
            "hidden transition retained stale foreground ready result");
    ForegroundLayoutReady hidden_ready;
    require(!runtime.try_take_layout_async(&hidden_ready),
            "hidden runtime delivered stale foreground result");
    require(
        runtime.request_layout_async(request(11U)) ==
            ForegroundLayoutWorkerScheduleResult::Inactive,
        "hidden runtime accepted foreground layout work");
    require(runtime.hot_scroll_stats().critical_trim_calls >= 1U,
            "critical hidden transition did not trim hot-scroll state");

    require(
        runtime.set_activity(
            FrameVisibility::Visible,
            FramePressure::Normal,
            4096,
            &error),
        "hidden-authority resume failed");
    require(
        runtime.request_layout_async(request(11U, 18U * 256U)) ==
            ForegroundLayoutWorkerScheduleResult::Accepted,
        "fresh resumed viewport request not accepted");
    require(foreground_pool.wait_idle_for(5s),
            "fresh resumed worker did not settle");

    ForegroundLayoutReady resumed;
    require(runtime.try_take_layout_async(&resumed),
            "fresh resumed foreground result not delivered");
    require(resumed.request_id == 11U && resumed.succeeded,
            "resumed foreground result identity or success changed");

    const ZenithTabRuntimeStats stats = runtime.stats();
    require(stats.critical_transitions == 1U,
            "critical transition accounting mismatch");
    require(stats.async_layout_rejections >= 1U,
            "hidden async rejection was not counted");
    require(stats.async_layout_success_drains == 1U,
            "only fresh resumed result should have been delivered");
}

} // namespace

int main() {
    const Fixture fixture = build_fixture();
    test_async_request_worker_delivery_and_sync_fence(fixture);
    test_hidden_authority_invalidates_ready_and_resumes_fresh(fixture);

    std::error_code ignored;
    std::filesystem::remove_all(fixture.root, ignored);
    require(!ignored, "async fixture cleanup failed");
    std::cout << "Zevryon async tab layout integration tests passed\n";
    return 0;
}
