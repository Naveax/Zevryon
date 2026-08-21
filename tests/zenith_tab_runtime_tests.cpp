#include "compact_document.hpp"
#include "layout_checkpoint.hpp"
#include "massivedoc_store.hpp"
#include "shared_source_prefetch_pool.hpp"
#include "zenith_hot_scroll.hpp"
#include "zenith_tab_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

struct PrefetchGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool first_entered{false};
    bool release_first{false};
    std::uint64_t calls{0U};
    std::vector<std::uint64_t> source_record_indices;
};

SharedSourcePrefetchExecutor gated_executor(
    const std::shared_ptr<PrefetchGate>& gate) {
    return [gate](
               const std::filesystem::path& root,
               std::uint64_t,
               const SourceWindowPrefetchRequest& request,
               std::vector<std::byte>* bytes,
               std::string* error) {
        if (bytes == nullptr || error == nullptr) {
            return false;
        }
        std::unique_lock<std::mutex> lock(gate->mutex);
        ++gate->calls;
        gate->source_record_indices.push_back(request.record_index);
        if (gate->calls == 1U) {
            gate->first_entered = true;
            gate->cv.notify_all();
            gate->cv.wait(lock, [gate] { return gate->release_first; });
        }
        lock.unlock();

        StoreReader reader(root);
        if (!reader.open(error)) {
            return false;
        }
        return reader.read_record_slice(
            request.record_index,
            request.byte_offset,
            request.max_bytes,
            bytes,
            error);
    };
}

void wait_first_entered(const std::shared_ptr<PrefetchGate>& gate) {
    std::unique_lock<std::mutex> lock(gate->mutex);
    require(
        gate->cv.wait_for(lock, 5s, [gate] { return gate->first_entered; }),
        "tab runtime prefetch did not enter executor");
}

void release_first(const std::shared_ptr<PrefetchGate>& gate) {
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->release_first = true;
    }
    gate->cv.notify_all();
}

struct Fixture {
    std::filesystem::path root;
    LayoutConfig layout;
    ArenaStats arena_stats;
};

Fixture build_fixture() {
    Fixture fixture;
    fixture.root =
        std::filesystem::temp_directory_path() / "zevryon-tab-runtime-tests";
    std::error_code ignored;
    std::filesystem::remove_all(fixture.root, ignored);

    constexpr std::uint64_t kRecordBytes = 512U * 1024U;
    StoreWriter writer(
        fixture.root,
        {.segment_bytes = 256U * 1024U, .records_per_search_block = 64U});
    std::string error;
    for (std::uint64_t record = 0U; record < 2U; ++record) {
        std::uint64_t generated = 0U;
        require(
            writer.append_stream(
                900U + record,
                kRecordBytes,
                [record, &generated](std::span<std::byte> target) {
                    for (std::size_t index = 0U; index < target.size(); ++index) {
                        const std::uint64_t absolute =
                            generated + static_cast<std::uint64_t>(index);
                        const unsigned char value =
                            (absolute % 80U) == 79U
                                ? static_cast<unsigned char>('\n')
                                : static_cast<unsigned char>(record == 0U ? 'a' : 'b');
                        target[index] = static_cast<std::byte>(value);
                    }
                    generated += static_cast<std::uint64_t>(target.size());
                    return target.size();
                },
                &error),
            "tab runtime fixture append failed");
        require(generated == kRecordBytes, "tab runtime fixture record incomplete");
    }

    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = 2U * kRecordBytes;
    metadata.logical_records = 2U;
    metadata.logical_nodes = 32U;
    metadata.style_runs = 8U;
    metadata.resource_references = 2U;
    metadata.largest_record_bytes = kRecordBytes;
    StoreStats store_stats;
    require(writer.finalize(metadata, &store_stats, &error),
            "tab runtime fixture finalize failed");

    ArenaConfig arena_config;
    arena_config.records_per_block = 1U;
    arena_config.estimated_bytes_per_line = 96U;
    arena_config.line_height_q8 = 18U * 256U;
    arena_config.vertical_padding_q8 = 12U * 256U;
    require(
        build_compact_arena(
            fixture.root,
            arena_config,
            &fixture.arena_stats,
            &error),
        "tab runtime compact arena build failed");

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
    for (std::uint64_t record = 0U; record < 2U; ++record) {
        LayoutCheckpointStats stats;
        require(
            build_layout_checkpoint(
                fixture.root,
                record,
                checkpoint,
                &stats,
                &error),
            "tab runtime checkpoint build failed");
        require(stats.record_index == record,
                "tab runtime checkpoint physical identity mismatch");
    }
    return fixture;
}

void test_hot_scroll_fragment_keeps_physical_identity_after_reorder(
    const Fixture& fixture) {
    ZenithHotScrollSession session(fixture.root, fixture.layout);
    std::string error;
    require(session.open(&error), "physical identity session open failed");

    LayoutWindowResult before;
    bool used_checkpoint = false;
    require(
        session.layout(
            0U,
            800U * 256U,
            720U * 256U,
            0U,
            128U,
            &before,
            &used_checkpoint,
            &error),
        "physical identity baseline layout failed");
    require(used_checkpoint && !before.fragments.empty(),
            "physical identity baseline missing fragments");
    require(before.fragments.front().record_index == 0U,
            "baseline logical ordinal mismatch");
    require(before.fragments.front().source_record_index == 0U,
            "baseline physical source identity mismatch");

    require(session.move_logical_record(0U, 1U, &error),
            "physical identity move failed");
    LayoutWindowResult moved;
    used_checkpoint = false;
    require(
        session.layout(
            0U,
            800U * 256U,
            720U * 256U,
            0U,
            128U,
            &moved,
            &used_checkpoint,
            &error),
        "physical identity moved layout failed");
    require(used_checkpoint && !moved.fragments.empty(),
            "physical identity moved layout missing fragments");
    require(moved.fragments.front().record_index == 0U,
            "moved fragment did not use new logical ordinal");
    require(moved.fragments.front().source_record_index == 1U,
            "moved fragment lost immutable physical source identity");
    require(moved.fragments.front().logical_id == 901U,
            "moved fragment logical id mismatch");

    require(session.move_logical_record(1U, 0U, &error),
            "physical identity restore move failed");
}

void test_exact_prefetch_cache_admission_bypasses_source_read(
    const Fixture& fixture) {
    ZenithHotScrollSession session(fixture.root, fixture.layout);
    std::string error;
    require(session.open(&error), "prefetch admission session open failed");
    session.clear_source_window_cache();

    StoreReader reader(fixture.root);
    require(reader.open(&error), "prefetch admission reader open failed");
    std::vector<std::byte> bytes;
    require(
        reader.read_record_slice(0U, 0U, kIoWindowBytes, &bytes, &error),
        "prefetch admission source read failed");
    require(bytes.size() == kIoWindowBytes,
            "prefetch admission fixture did not return exact window");

    std::vector<std::byte> short_bytes(bytes.begin(), bytes.end() - 1);
    require(
        !session.admit_prefetched_source_window(
            0U,
            0U,
            kIoWindowBytes,
            std::move(short_bytes)),
        "partial speculative window was admitted");
    require(
        session.admit_prefetched_source_window(
            0U,
            0U,
            kIoWindowBytes,
            std::move(bytes)),
        "exact speculative window was not admitted");

    LayoutWindowResult result;
    bool used_checkpoint = false;
    require(
        session.layout(
            0U,
            800U * 256U,
            720U * 256U,
            0U,
            128U,
            &result,
            &used_checkpoint,
            &error),
        "prefetch admission layout failed");
    require(used_checkpoint && !result.fragments.empty(),
            "prefetch admission layout missing fragments");
    require(result.source_bytes_read == 0U,
            "exact prefetched window did not bypass synchronous source read");
    require(result.source_window_cache_hits >= 1U,
            "exact prefetched window did not register a source-cache hit");
    require(result.source_window_cache_misses == 0U,
            "exact prefetched window unexpectedly missed source cache");
}

void test_tab_runtime_hidden_suppression_and_shared_prefetch(
    const Fixture& fixture) {
    const auto gate = std::make_shared<PrefetchGate>();
    SharedSourcePrefetchPool pool(
        {1U, 256U * 1024U},
        gated_executor(gate));

    ZenithTabRuntimeConfig config;
    config.layout = fixture.layout;
    config.frame_budget = FrameBudgetPolicy{
        1'000'000U,
        100'000U,
        10'000U,
        5'000U};
    config.prefetch_reserve_us = 100U;
    config.prefetch_bytes = kIoWindowBytes;

    ZenithTabRuntime runtime(fixture.root, &pool, 77U, config);
    std::string error;
    require(runtime.open(&error), "tab runtime open failed");
    require(pool.status().live_threads == 0U,
            "tab runtime registration eagerly started shared workers");
    require(
        runtime.set_activity(
            FrameVisibility::Visible,
            FramePressure::Normal,
            4096,
            &error),
        "tab runtime visible activity update failed");

    LayoutWindowResult first;
    bool used_checkpoint = false;
    require(
        runtime.layout(
            0U,
            800U * 256U,
            720U * 256U,
            720U * 256U,
            128U,
            &first,
            &used_checkpoint,
            &error),
        "tab runtime visible layout failed");
    require(used_checkpoint && !first.fragments.empty(),
            "tab runtime visible layout missing accelerated fragments");
    require(runtime.stats().prefetch_schedule_accepts == 1U,
            "tab runtime did not schedule first shared prefetch");
    wait_first_entered(gate);

    const std::uint64_t hot_layouts_before_hidden =
        runtime.hot_scroll_stats().layout_calls;
    require(
        runtime.set_activity(
            FrameVisibility::Hidden,
            FramePressure::Elevated,
            4096,
            &error),
        "tab runtime hidden transition failed");
    require(runtime.hot_scroll_stats().background_trim_calls == 1U,
            "hidden transition did not trim hot-scroll working set");

    LayoutWindowResult hidden;
    used_checkpoint = true;
    require(
        runtime.layout(
            0U,
            800U * 256U,
            720U * 256U,
            720U * 256U,
            128U,
            &hidden,
            &used_checkpoint,
            &error),
        "hidden tab layout suppression call failed");
    require(hidden.fragments.empty() && !used_checkpoint,
            "hidden tab returned rendered work");
    require(runtime.stats().hidden_layout_suppressions == 1U,
            "hidden layout suppression not counted");
    require(runtime.hot_scroll_stats().layout_calls == hot_layouts_before_hidden,
            "hidden layout entered underlying hot-scroll engine");

    release_first(gate);
    require(pool.wait_idle_for(5s), "hidden prefetch did not settle");
    require(pool.status().inactive_results_dropped == 1U,
            "running prefetch result survived hidden transition");

    const PrefetchTicket hidden_ticket = runtime.prefetch_ticket();
    require(hidden_ticket.direction == 0,
            "hidden runtime did not neutralize prefetch direction");
    require(
        runtime.set_activity(
            FrameVisibility::Visible,
            FramePressure::Normal,
            4096,
            &error),
        "tab runtime visible resume failed");
    const PrefetchTicket resumed = runtime.prefetch_ticket();
    require(resumed.direction == 1 && resumed.epoch > hidden_ticket.epoch,
            "visible resume did not create fresh prefetch authority");

    LayoutWindowResult resumed_layout;
    used_checkpoint = false;
    require(
        runtime.layout(
            18U * 256U,
            800U * 256U,
            720U * 256U,
            720U * 256U,
            128U,
            &resumed_layout,
            &used_checkpoint,
            &error),
        "tab runtime resumed layout failed");
    require(pool.wait_idle_for(5s), "resumed prefetch did not settle");
    require(pool.status().ready_results == 1U,
            "resumed prefetch did not publish bounded ready result");

    LayoutWindowResult drain_layout;
    used_checkpoint = false;
    require(
        runtime.layout(
            36U * 256U,
            800U * 256U,
            720U * 256U,
            720U * 256U,
            128U,
            &drain_layout,
            &used_checkpoint,
            &error),
        "tab runtime ready-drain layout failed");
    require(runtime.stats().prefetch_ready_drains >= 1U,
            "tab runtime did not drain shared result");
    require(runtime.stats().prefetch_ready_bytes_drained >= config.prefetch_bytes,
            "tab runtime drained-byte accounting mismatch");
    require(runtime.stats().prefetch_cache_admissions >= 1U,
            "successful shared prefetch was not admitted into hot-scroll cache");
    require(runtime.stats().prefetch_cache_rejections == 0U,
            "exact shared prefetch was rejected from hot-scroll cache");

    require(
        runtime.set_activity(
            FrameVisibility::Hidden,
            FramePressure::Critical,
            0,
            &error),
        "tab runtime critical hidden transition failed");
    require(runtime.hot_scroll_stats().critical_trim_calls == 1U,
            "critical hidden transition did not release checkpoint state");
    require(runtime.hot_scroll_stats().source_window_cache_bytes == 0U,
            "critical hidden transition retained source cache");
    require(runtime.hot_scroll_stats().checkpoint_cache_bytes == 0U,
            "critical hidden transition retained checkpoint cache");

    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        require(!gate->source_record_indices.empty(),
                "tab runtime executor saw no physical source request");
        require(gate->source_record_indices.front() == 0U,
                "tab runtime prefetch used logical ordinal instead of physical identity");
    }
}

} // namespace

int main() {
    const Fixture fixture = build_fixture();
    test_hot_scroll_fragment_keeps_physical_identity_after_reorder(fixture);
    test_exact_prefetch_cache_admission_bypasses_source_read(fixture);
    test_tab_runtime_hidden_suppression_and_shared_prefetch(fixture);

    std::error_code ignored;
    std::filesystem::remove_all(fixture.root, ignored);
    require(!ignored, "tab runtime fixture cleanup failed");
    std::cout << "Zevryon tab-runtime integration tests passed\n";
    return 0;
}
