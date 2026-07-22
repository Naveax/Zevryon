#include "compact_document.hpp"
#include "layout_checkpoint.hpp"
#include "massivedoc_store.hpp"
#include "zenith_layout_window.hpp"

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
        std::filesystem::temp_directory_path() / "zevryon-zenith-layout-tests";
    std::error_code fs_error;
    std::filesystem::remove_all(root, fs_error);
    fs_error.clear();

    constexpr std::uint64_t kRecordBytes = 8ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t kLogicalId = 991U;
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
        !require(generated == kRecordBytes, "zenith fixture generated completely")) {
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
    layout_config.checkpoint_stride_bytes = 64U * 1024U;
    layout_config.checkpoint_min_record_bytes = 1024U * 1024U;
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

    const std::uint64_t viewport_height = 720U * 256U;
    const std::uint64_t scroll_y = arena_stats.total_height_q8 / 2U;
    zevryon::massivedoc::LayoutWindowResult accelerated;
    bool used_checkpoint = false;
    if (!require(
            zevryon::massivedoc::layout_window_with_persistent_checkpoints(
                root,
                layout_config,
                scroll_y,
                800U * 256U,
                viewport_height,
                720U * 256U,
                256U,
                &accelerated,
                &used_checkpoint,
                &error),
            error) ||
        !require(used_checkpoint, "normal layout selects persistent checkpoint path") ||
        !require(accelerated.checkpoint_accelerated, "result marks checkpoint acceleration") ||
        !require(accelerated.checkpoint_hits >= 2U, "measurement and fragment passes hit checkpoint") ||
        !require(
            accelerated.checkpoint_index_bytes == checkpoint_stats.physical_bytes,
            "checkpoint resident accounting is unique") ||
        !require(!accelerated.fragments.empty(), "accelerated layout returns fragments") ||
        !require(
            accelerated.source_bytes_read <= 2U * 64U * 1024U,
            "normal layout remains within two I/O windows") ||
        !require(
            accelerated.source_bytes_read < kRecordBytes / 16U,
            "normal layout avoids full-record scan")) {
        return 1;
    }

    for (const auto& fragment : accelerated.fragments) {
        if (!require(fragment.record_index == 0U, "accelerated fragment identity") ||
            !require(fragment.source_end > fragment.source_start, "accelerated fragment advances")) {
            return 1;
        }
    }

    const auto checkpoint_path =
        zevryon::massivedoc::layout_checkpoint_path(root, 0U, checkpoint_config);
    if (!require(std::filesystem::remove(checkpoint_path, fs_error), "remove checkpoint for fallback") ||
        !require(!fs_error, "checkpoint removal error")) {
        return 1;
    }
    zevryon::massivedoc::LayoutWindowResult fallback_probe;
    bool fallback_used_checkpoint = true;
    error.clear();
    if (!require(
            zevryon::massivedoc::layout_window_with_persistent_checkpoints(
                root,
                layout_config,
                scroll_y,
                800U * 256U,
                viewport_height,
                720U * 256U,
                256U,
                &fallback_probe,
                &fallback_used_checkpoint,
                &error),
            error) ||
        !require(!fallback_used_checkpoint, "missing checkpoint selects safe legacy fallback")) {
        return 1;
    }

    zevryon::massivedoc::StoreReader reader(root);
    if (!require(reader.open(&error), error) ||
        !require(reader.verify(&error), error)) {
        return 1;
    }

    std::filesystem::remove_all(root, fs_error);
    if (!require(!fs_error, "zenith layout test cleanup")) {
        return 1;
    }
    std::cout << "zenith layout window tests passed\n";
    return 0;
}
