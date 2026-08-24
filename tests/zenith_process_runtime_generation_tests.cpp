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
                   "zevryon-runtime-generation-tests";
    std::error_code ignored;
    std::filesystem::remove_all(fixture.root, ignored);

    constexpr std::uint64_t kRecordBytes = 4U * 1024U * 1024U;
    StoreWriter writer(
        fixture.root,
        {.segment_bytes = 1U * 1024U * 1024U, .records_per_search_block = 64U});
    std::string error;
    std::uint64_t generated = 0U;
    require(
        writer.append_stream(
            9901U,
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
        "generation fixture append failed");
    require(generated == kRecordBytes, "generation fixture record incomplete");

    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = kRecordBytes;
    metadata.logical_records = 1U;
    metadata.logical_nodes = 16U;
    metadata.largest_record_bytes = kRecordBytes;
    StoreStats store_stats;
    require(writer.finalize(metadata, &store_stats, &error),
            "generation fixture finalize failed");

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
        "generation compact arena build failed");

    fixture.layout.checkpoint_stride_bytes = 16U * 1024U;
    fixture.layout.checkpoint_min_record_bytes = 1U;
    fixture.layout.max_checkpoint_cache_bytes = 512U * 1024U;
    fixture.layout.max_source_window_cache_bytes = 256U * 1024U;

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
        "generation checkpoint build failed");
    return fixture;
}

ForegroundLayoutRequest long_layout_request(std::uint64_t request_id) {
    ForegroundLayoutRequest request;
    request.request_id = request_id;
    request.scroll_y_q8 = 0U;
    request.viewport_width_q8 = 800U * 256U;
    request.viewport_height_q8 = 100'000ULL * 18ULL * 256ULL;
    request.overscan_q8 = 0U;
    request.max_fragments = 60'000U;
    return request;
}

bool wait_for_running(
    ZenithProcessRuntimeServices* services,
    std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (services->status().foreground_layout_pool.running_sessions != 0U) {
            return true;
        }
        std::this_thread::yield();
    }
    return services->status().foreground_layout_pool.running_sessions != 0U;
}

bool wait_for_idle(
    ZenithProcessRuntimeServices* services,
    std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (services->status().foreground_layout_pool.running_sessions == 0U) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return services->status().foreground_layout_pool.running_sessions == 0U;
}

void test_close_retires_running_generation_and_reuses_public_identity(
    const Fixture& fixture) {
    ZenithProcessRuntimeServicesConfig config;
    config.prefetch_worker_count = 1U;
    config.prefetch_ready_bytes = 128U * 1024U;
    config.foreground_layout_worker_count = 1U;
    config.foreground_layout_ready_bytes = 16U * 1024U * 1024U;
    config.foreground_layout_max_fragments = 60'000U;
    ZenithProcessRuntimeServices services(config);
    require(services.valid(), "generation process services invalid");

    std::string error;
    constexpr std::uint64_t kPublicSession = 4401U;
    require(
        services.open_tab(
            kPublicSession,
            fixture.root,
            DeviceFrameProfile::Desktop,
            fixture.layout,
            FrameVisibility::Visible,
            0,
            &error),
        "generation visible tab open failed");
    require(
        services.request_tab_layout_async(
            kPublicSession,
            long_layout_request(1U)) ==
            ForegroundLayoutWorkerScheduleResult::Accepted,
        "generation long foreground request not accepted");
    require(wait_for_running(&services, 5s),
            "generation foreground callback never entered running state");

    const auto close_started = std::chrono::steady_clock::now();
    require(services.close_tab(kPublicSession),
            "running generation close failed");
    const auto close_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - close_started);
    require(close_elapsed < 250ms,
            "close waited for running foreground generation");

    auto status = services.status();
    require(status.tabs == 0U && status.materialized_tabs == 0U,
            "closed public tab remained materialized");
    require(status.retired_runtime_generations >= 1U,
            "running runtime generation was destroyed synchronously");

    error.clear();
    require(
        services.open_tab(
            kPublicSession,
            fixture.root,
            DeviceFrameProfile::Desktop,
            fixture.layout,
            FrameVisibility::Visible,
            0,
            &error),
        "public session identity could not be reused beside retired generation");
    require(services.tab_materialized(kPublicSession),
            "reused public session did not receive a fresh runtime generation");

    require(wait_for_idle(&services, 10s),
            "retired foreground generation did not finish");
    require(
        services.set_tab_activity(
            kPublicSession,
            FrameVisibility::Visible,
            0,
            &error),
        "reused public session activity refresh failed");
    status = services.status();
    require(status.retired_runtime_generations == 0U,
            "finished retired generation was not reclaimed");
    require(status.foreground_layout_pool.sessions == 1U,
            "old internal foreground session survived generation reclamation");

    require(services.close_tab(kPublicSession),
            "reused public session close failed");
    status = services.status();
    require(status.tabs == 0U && status.materialized_tabs == 0U &&
                status.retired_runtime_generations == 0U &&
                status.foreground_layout_pool.sessions == 0U &&
                status.prefetch_pool.sessions == 0U,
            "generation close retained process runtime resources");
}

} // namespace

int main() {
    const Fixture fixture = build_fixture();
    test_close_retires_running_generation_and_reuses_public_identity(fixture);

    std::error_code ignored;
    std::filesystem::remove_all(fixture.root, ignored);
    require(!ignored, "generation fixture cleanup failed");
    std::cout << "Zevryon runtime generation retirement tests passed\n";
    return 0;
}
