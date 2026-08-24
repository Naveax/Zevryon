#include "compact_document.hpp"
#include "layout_checkpoint.hpp"
#include "massivedoc_store.hpp"
#include "zenith_hot_scroll.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>

namespace {

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

} // namespace

int main() {
    const auto root =
        std::filesystem::temp_directory_path() / "zevryon-hot-scroll-nonblocking-tests";
    std::error_code fs_error;
    std::filesystem::remove_all(root, fs_error);
    fs_error.clear();

    constexpr std::uint64_t kRecordBytes = 1024ULL * 1024ULL;
    constexpr std::uint64_t kLogicalId = 4242U;

    zevryon::massivedoc::StoreWriter writer(
        root, {.segment_bytes = 1024U * 1024U, .records_per_search_block = 64U});
    std::uint64_t generated = 0U;
    std::string error;
    if (!require(
            writer.append_stream(
                kLogicalId,
                kRecordBytes,
                [&generated](std::span<std::byte> target) {
                    std::fill(
                        target.begin(),
                        target.end(),
                        static_cast<std::byte>(static_cast<unsigned char>('x')));
                    generated += static_cast<std::uint64_t>(target.size());
                    return target.size();
                },
                &error),
            error) ||
        !require(generated == kRecordBytes, "fixture generated completely")) {
        return 1;
    }

    zevryon::massivedoc::CorpusMetadata metadata;
    metadata.logical_utf8_bytes = kRecordBytes;
    metadata.logical_records = 1U;
    metadata.logical_nodes = 1U;
    metadata.style_runs = 0U;
    metadata.resource_references = 0U;
    metadata.largest_record_bytes = kRecordBytes;
    zevryon::massivedoc::StoreStats store_stats;
    if (!require(writer.finalize(metadata, &store_stats, &error), error)) {
        return 1;
    }

    zevryon::massivedoc::ArenaConfig arena_config;
    arena_config.records_per_block = 1U;
    arena_config.estimated_bytes_per_line = 96U;
    arena_config.line_height_q8 = 18U * 256U;
    arena_config.vertical_padding_q8 = 12U * 256U;
    zevryon::massivedoc::ArenaStats arena_stats;
    if (!require(
            zevryon::massivedoc::build_compact_arena(
                root, arena_config, &arena_stats, &error),
            error)) {
        return 1;
    }

    zevryon::massivedoc::LayoutConfig layout_config;
    layout_config.checkpoint_stride_bytes = 16U * 1024U;
    layout_config.checkpoint_min_record_bytes = 1U;
    layout_config.max_checkpoint_cache_bytes = 256U * 1024U;
    layout_config.max_source_window_cache_bytes = 128U * 1024U;

    zevryon::massivedoc::LayoutCheckpointConfig checkpoint_config;
    checkpoint_config.width_q8 = 800U * 256U;
    checkpoint_config.average_advance_q8 = layout_config.average_advance_q8;
    checkpoint_config.line_height_q8 = layout_config.line_height_q8;
    checkpoint_config.horizontal_padding_q8 = layout_config.horizontal_padding_q8;
    checkpoint_config.vertical_padding_q8 = layout_config.vertical_padding_q8;
    checkpoint_config.target_stride_bytes = layout_config.checkpoint_stride_bytes;
    zevryon::massivedoc::LayoutCheckpointStats checkpoint_stats;
    if (!require(
            zevryon::massivedoc::build_layout_checkpoint(
                root, 0U, checkpoint_config, &checkpoint_stats, &error),
            error)) {
        return 1;
    }

    zevryon::massivedoc::ZenithHotScrollSession session(root, layout_config);
    if (!require(session.open(&error), error)) {
        return 1;
    }

    constexpr std::uint64_t kViewportHeightQ8 = 720U * 256U;
    constexpr std::uint32_t kViewportWidthQ8 = 800U * 256U;

    zevryon::massivedoc::LayoutWindowResult result;
    bool used_checkpoint = false;
    auto status = zevryon::massivedoc::ZenithHotScrollLayoutStatus::Ready;
    if (!require(
            session.layout_nonblocking(
                0U,
                kViewportWidthQ8,
                kViewportHeightQ8,
                0U,
                256U,
                &result,
                &used_checkpoint,
                &status,
                &error),
            "fresh cache-only layout returns a readiness result") ||
        !require(error.empty(), "checkpoint would-block is not a hard error") ||
        !require(
            status == zevryon::massivedoc::ZenithHotScrollLayoutStatus::WouldBlockCheckpoint,
            "fresh cache-only layout refuses checkpoint filesystem access") ||
        !require(!used_checkpoint, "checkpoint would-block publishes no completed layout") ||
        !require(result.source_bytes_read == 0U,
                 "checkpoint would-block performs zero source I/O")) {
        return 1;
    }

    result = {};
    used_checkpoint = false;
    if (!require(
            session.layout(
                0U,
                kViewportWidthQ8,
                kViewportHeightQ8,
                0U,
                256U,
                &result,
                &used_checkpoint,
                &error),
            error) ||
        !require(used_checkpoint, "explicit blocking warm-up loads checkpoint") ||
        !require(!result.fragments.empty(), "blocking warm-up returns fragments") ||
        !require(result.source_bytes_read > 0U,
                 "blocking warm-up performs bounded source I/O")) {
        return 1;
    }

    zevryon::massivedoc::LayoutWindowResult cached;
    used_checkpoint = false;
    status = zevryon::massivedoc::ZenithHotScrollLayoutStatus::WouldBlockCheckpoint;
    if (!require(
            session.layout_nonblocking(
                0U,
                kViewportWidthQ8,
                kViewportHeightQ8,
                0U,
                256U,
                &cached,
                &used_checkpoint,
                &status,
                &error),
            "warm cache-only layout succeeds") ||
        !require(error.empty(), "warm cache-only layout has no error") ||
        !require(
            status == zevryon::massivedoc::ZenithHotScrollLayoutStatus::Ready,
            "warm cache-only layout reports Ready") ||
        !require(used_checkpoint, "warm cache-only layout uses resident checkpoint") ||
        !require(cached.source_bytes_read == 0U,
                 "warm cache-only layout performs zero physical source I/O") ||
        !require(!cached.fragments.empty(), "warm cache-only layout preserves fragments")) {
        return 1;
    }

    session.clear_source_window_cache();
    zevryon::massivedoc::LayoutWindowResult source_miss;
    used_checkpoint = false;
    status = zevryon::massivedoc::ZenithHotScrollLayoutStatus::Ready;
    if (!require(
            session.layout_nonblocking(
                0U,
                kViewportWidthQ8,
                kViewportHeightQ8,
                0U,
                256U,
                &source_miss,
                &used_checkpoint,
                &status,
                &error),
            "source cache miss returns readiness result") ||
        !require(error.empty(), "source would-block is not a hard error") ||
        !require(
            status == zevryon::massivedoc::ZenithHotScrollLayoutStatus::WouldBlockSource,
            "cache-only layout refuses StoreReader fallback") ||
        !require(!used_checkpoint, "source would-block publishes no completed layout") ||
        !require(source_miss.source_bytes_read == 0U,
                 "source would-block performs zero physical source I/O")) {
        return 1;
    }

    zevryon::massivedoc::LayoutWindowResult fallback;
    used_checkpoint = false;
    if (!require(
            session.layout(
                0U,
                kViewportWidthQ8,
                kViewportHeightQ8,
                0U,
                256U,
                &fallback,
                &used_checkpoint,
                &error),
            error) ||
        !require(used_checkpoint, "legacy synchronous fallback remains usable") ||
        !require(fallback.source_bytes_read > 0U,
                 "legacy synchronous fallback can refill the source cache") ||
        !require(!fallback.fragments.empty(), "legacy fallback preserves correctness")) {
        return 1;
    }

    session.trim_memory(zevryon::massivedoc::ZenithMemoryPressure::Critical);
    zevryon::massivedoc::LayoutWindowResult critical_miss;
    used_checkpoint = false;
    status = zevryon::massivedoc::ZenithHotScrollLayoutStatus::Ready;
    if (!require(
            session.layout_nonblocking(
                0U,
                kViewportWidthQ8,
                kViewportHeightQ8,
                0U,
                256U,
                &critical_miss,
                &used_checkpoint,
                &status,
                &error),
            "critical-trim cache-only layout returns readiness result") ||
        !require(
            status == zevryon::massivedoc::ZenithHotScrollLayoutStatus::WouldBlockCheckpoint,
            "critical trim forces checkpoint worker handoff before UI layout") ||
        !require(critical_miss.source_bytes_read == 0U,
                 "critical-trim checkpoint miss performs zero source I/O")) {
        return 1;
    }

    std::filesystem::remove_all(root, fs_error);
    std::cout << "hot-scroll non-blocking tests passed\n";
    return 0;
}
