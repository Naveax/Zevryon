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
        std::filesystem::temp_directory_path() / "zevryon-hot-scroll-tests";
    std::error_code fs_error;
    std::filesystem::remove_all(root, fs_error);
    fs_error.clear();

    constexpr std::uint64_t kRecordBytes = 8ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t kLogicalId = 2026U;
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
        !require(generated == kRecordBytes, "hot-scroll fixture generated completely")) {
        return 1;
    }

    zevryon::massivedoc::CorpusMetadata metadata;
    metadata.logical_utf8_bytes = kRecordBytes;
    metadata.logical_records = 1U;
    metadata.logical_nodes = 8U;
    metadata.style_runs = 4U;
    metadata.resource_references = 1U;
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
            error) ||
        !require(
            checkpoint_stats.physical_bytes < kRecordBytes / 500U,
            "16 KiB checkpoint overhead remains below 0.2 percent")) {
        return 1;
    }

    zevryon::massivedoc::ZenithHotScrollSession session(root, layout_config);
    if (!require(session.open(&error), error)) {
        return 1;
    }

    const std::uint64_t viewport_height_q8 = 720U * 256U;
    const std::uint64_t overscan_q8 = 720U * 256U;
    const std::uint64_t scroll_y_q8 = arena_stats.total_height_q8 / 2U;
    zevryon::massivedoc::LayoutWindowResult first;
    bool used_checkpoint = false;
    if (!require(
            session.layout(
                scroll_y_q8,
                800U * 256U,
                viewport_height_q8,
                overscan_q8,
                256U,
                &first,
                &used_checkpoint,
                &error),
            error) ||
        !require(used_checkpoint, "first hot-scroll query uses checkpoints") ||
        !require(first.checkpoint_accelerated, "first hot-scroll result is accelerated") ||
        !require(!first.fragments.empty(), "first hot-scroll query returns fragments") ||
        !require(first.source_bytes_read <= 64U * 1024U, "16 KiB checkpoint needs one I/O window") ||
        !require(first.checkpoint_cache_misses >= 1U, "first query records checkpoint cache miss") ||
        !require(first.checkpoint_cache_hits >= 1U, "second pass reuses checkpoint cache") ||
        !require(first.source_window_cache_misses >= 1U, "first query loads source window") ||
        !require(
            first.checkpoint_cache_bytes <= layout_config.max_checkpoint_cache_bytes,
            "checkpoint cache respects byte budget") ||
        !require(
            first.source_window_cache_bytes <= layout_config.max_source_window_cache_bytes,
            "source cache respects byte budget")) {
        return 1;
    }

    zevryon::massivedoc::LayoutWindowResult repeated;
    used_checkpoint = false;
    if (!require(
            session.layout(
                scroll_y_q8,
                800U * 256U,
                viewport_height_q8,
                overscan_q8,
                256U,
                &repeated,
                &used_checkpoint,
                &error),
            error) ||
        !require(used_checkpoint, "repeated hot-scroll query uses checkpoints") ||
        !require(repeated.source_bytes_read == 0U, "identical query performs zero source I/O") ||
        !require(repeated.checkpoint_cache_hits >= 2U, "repeated query reuses checkpoint in both passes") ||
        !require(repeated.checkpoint_cache_misses == 0U, "repeated query has no checkpoint miss") ||
        !require(repeated.source_window_cache_hits >= 1U, "repeated query reuses source window") ||
        !require(repeated.source_window_cache_misses == 0U, "repeated query has no source miss") ||
        !require(
            repeated.fragments.size() == first.fragments.size(),
            "cached query preserves fragment count")) {
        return 1;
    }

    zevryon::massivedoc::LayoutWindowResult adjacent;
    used_checkpoint = false;
    if (!require(
            session.layout(
                scroll_y_q8 + 18U * 256U,
                800U * 256U,
                viewport_height_q8,
                overscan_q8,
                256U,
                &adjacent,
                &used_checkpoint,
                &error),
            error) ||
        !require(used_checkpoint, "adjacent scroll remains checkpoint accelerated") ||
        !require(adjacent.source_bytes_read == 0U, "adjacent line scroll reuses source window") ||
        !require(adjacent.source_window_cache_hits >= 1U, "adjacent scroll reports source cache hit")) {
        return 1;
    }

    const auto cumulative = session.stats();
    if (!require(cumulative.layout_calls == 3U, "session records layout calls") ||
        !require(
            cumulative.checkpoint_cache_peak_bytes <= layout_config.max_checkpoint_cache_bytes,
            "checkpoint peak charge stays bounded") ||
        !require(
            cumulative.source_window_cache_peak_bytes <= layout_config.max_source_window_cache_bytes,
            "source-window peak charge stays bounded")) {
        return 1;
    }

    const auto checkpoint_path =
        zevryon::massivedoc::layout_checkpoint_path(root, 0U, checkpoint_config);
    if (!require(std::filesystem::remove(checkpoint_path, fs_error), "remove checkpoint for fallback") ||
        !require(!fs_error, "checkpoint removal error")) {
        return 1;
    }
    zevryon::massivedoc::ZenithHotScrollSession fallback_session(root, layout_config);
    if (!require(fallback_session.open(&error), error)) {
        return 1;
    }
    zevryon::massivedoc::LayoutWindowResult fallback;
    used_checkpoint = true;
    if (!require(
            fallback_session.layout(
                scroll_y_q8,
                800U * 256U,
                viewport_height_q8,
                overscan_q8,
                256U,
                &fallback,
                &used_checkpoint,
                &error),
            error) ||
        !require(!used_checkpoint, "missing checkpoint selects safe fallback")) {
        return 1;
    }

    zevryon::massivedoc::StoreReader reader(root);
    if (!require(reader.open(&error), error) ||
        !require(reader.verify(&error), error)) {
        return 1;
    }

    std::filesystem::remove_all(root, fs_error);
    if (!require(!fs_error, "hot-scroll test cleanup")) {
        return 1;
    }
    std::cout << "ZENITH hot-scroll tests passed\n";
    return 0;
}
