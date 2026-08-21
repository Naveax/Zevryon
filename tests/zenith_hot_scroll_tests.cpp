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
    constexpr std::uint64_t kSecondRecordBytes = 256ULL * 1024ULL;
    constexpr std::uint64_t kLogicalId = 2026U;
    constexpr std::uint64_t kSecondLogicalId = 2027U;
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

    std::uint64_t second_generated = 0U;
    if (!require(
            writer.append_stream(
                kSecondLogicalId,
                kSecondRecordBytes,
                [&second_generated](std::span<std::byte> target) {
                    for (std::size_t index = 0U; index < target.size(); ++index) {
                        const std::uint64_t absolute =
                            second_generated + static_cast<std::uint64_t>(index);
                        const char value = (absolute % 64U) == 63U ? '\n' : 'y';
                        target[index] = static_cast<std::byte>(
                            static_cast<unsigned char>(value));
                    }
                    second_generated += static_cast<std::uint64_t>(target.size());
                    return target.size();
                },
                &error),
            error) ||
        !require(second_generated == kSecondRecordBytes,
                 "second hot-scroll fixture generated completely")) {
        return 1;
    }

    zevryon::massivedoc::CorpusMetadata metadata;
    metadata.logical_utf8_bytes = kRecordBytes + kSecondRecordBytes;
    metadata.logical_records = 2U;
    metadata.logical_nodes = 16U;
    metadata.style_runs = 8U;
    metadata.resource_references = 2U;
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
            error) ||
        !require(
            checkpoint_stats.physical_bytes < kRecordBytes / 500U,
            "16 KiB checkpoint overhead remains below 0.2 percent")) {
        return 1;
    }
    zevryon::massivedoc::LayoutCheckpointStats second_checkpoint_stats;
    if (!require(
            zevryon::massivedoc::build_layout_checkpoint(
                root, 1U, checkpoint_config, &second_checkpoint_stats, &error),
            error) ||
        !require(second_checkpoint_stats.logical_id == kSecondLogicalId,
                 "second checkpoint preserves physical source identity")) {
        return 1;
    }

    zevryon::massivedoc::ZenithHotScrollSession session(root, layout_config);
    if (!require(session.open(&error), error)) {
        return 1;
    }

    const std::uint64_t viewport_height_q8 = 720U * 256U;
    const std::uint64_t overscan_q8 = 720U * 256U;
    const std::uint64_t scroll_y_q8 = arena_stats.total_height_q8 / 2U;

    zevryon::massivedoc::LayoutWindowResult calibration;
    bool used_checkpoint = false;
    if (!require(
            session.layout(
                scroll_y_q8,
                800U * 256U,
                viewport_height_q8,
                overscan_q8,
                256U,
                &calibration,
                &used_checkpoint,
                &error),
            error) ||
        !require(used_checkpoint, "calibration query uses checkpoints") ||
        !require(calibration.checkpoint_accelerated, "calibration corrects arena geometry") ||
        !require(!calibration.fragments.empty(), "calibration returns fragments")) {
        return 1;
    }
    session.clear_source_window_cache();

    zevryon::massivedoc::LayoutWindowResult first;
    used_checkpoint = false;
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
        !require(used_checkpoint, "stable cold query uses checkpoints") ||
        !require(first.checkpoint_accelerated, "stable cold result is accelerated") ||
        !require(!first.fragments.empty(), "stable cold query returns fragments") ||
        !require(first.source_bytes_read <= 64U * 1024U, "16 KiB checkpoint needs one I/O window") ||
        !require(first.checkpoint_cache_misses == 0U, "stable cold query reuses parsed checkpoint") ||
        !require(first.checkpoint_cache_hits >= 2U, "stable cold query hits checkpoint in both passes") ||
        !require(first.source_window_cache_misses >= 1U, "stable cold query loads source window") ||
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
        !require(repeated.source_bytes_read == 0U, "identical stable query performs zero source I/O") ||
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
    if (!require(cumulative.layout_calls == 4U, "session records calibration and measured calls") ||
        !require(
            cumulative.checkpoint_cache_peak_bytes <= layout_config.max_checkpoint_cache_bytes,
            "checkpoint peak charge stays bounded") ||
        !require(
            cumulative.source_window_cache_peak_bytes <= layout_config.max_source_window_cache_bytes,
            "source-window peak charge stays bounded") ||
        !require(cumulative.checkpoint_cache_bytes > 0U,
                 "hot tab retains parsed checkpoint state") ||
        !require(cumulative.source_window_cache_bytes > 0U,
                 "hot tab retains source-window state") ||
        !require(cumulative.source_scratch_capacity_bytes > 0U,
                 "source scratch capacity is visible in telemetry") ||
        !require(cumulative.fragment_scratch_capacity_bytes > 0U,
                 "fragment scratch capacity is visible in telemetry")) {
        return 1;
    }

    session.trim_memory(zevryon::massivedoc::ZenithMemoryPressure::Background);
    const auto after_background_trim = session.stats();
    if (!require(after_background_trim.background_trim_calls == 1U,
                 "background trim is counted") ||
        !require(after_background_trim.critical_trim_calls == 0U,
                 "background trim does not count as critical") ||
        !require(after_background_trim.source_window_cache_bytes == 0U,
                 "background tab releases source-window cache") ||
        !require(after_background_trim.source_scratch_capacity_bytes == 0U,
                 "background tab releases source scratch") ||
        !require(after_background_trim.fragment_scratch_capacity_bytes == 0U,
                 "background tab releases fragment scratch") ||
        !require(
            after_background_trim.checkpoint_cache_bytes == cumulative.checkpoint_cache_bytes,
            "background tab preserves parsed checkpoints for fast resume") ||
        !require(after_background_trim.trim_reclaimed_bytes > 0U,
                 "background trim reports reclaimed working-set bytes")) {
        return 1;
    }

    zevryon::massivedoc::LayoutWindowResult background_resume;
    used_checkpoint = false;
    if (!require(
            session.layout(
                scroll_y_q8,
                800U * 256U,
                viewport_height_q8,
                overscan_q8,
                256U,
                &background_resume,
                &used_checkpoint,
                &error),
            error) ||
        !require(used_checkpoint, "background tab resumes on checkpoint path") ||
        !require(background_resume.checkpoint_cache_misses == 0U,
                 "background resume keeps parsed checkpoint hot") ||
        !require(background_resume.checkpoint_cache_hits >= 2U,
                 "background resume reuses checkpoint in both passes") ||
        !require(background_resume.source_window_cache_misses >= 1U,
                 "background resume reloads only bounded source window") ||
        !require(background_resume.source_bytes_read > 0U &&
                     background_resume.source_bytes_read <= 64U * 1024U,
                 "background resume performs at most one bounded source read") ||
        !require(background_resume.fragments.size() == first.fragments.size(),
                 "background trim preserves rendered fragment result")) {
        return 1;
    }

    const std::uint64_t reclaimed_after_background =
        session.stats().trim_reclaimed_bytes;
    session.trim_memory(zevryon::massivedoc::ZenithMemoryPressure::Critical);
    const auto after_critical_trim = session.stats();
    if (!require(after_critical_trim.background_trim_calls == 1U,
                 "critical trim preserves background trim count") ||
        !require(after_critical_trim.critical_trim_calls == 1U,
                 "critical trim is counted") ||
        !require(after_critical_trim.checkpoint_cache_bytes == 0U,
                 "critical pressure releases parsed checkpoints") ||
        !require(after_critical_trim.source_window_cache_bytes == 0U,
                 "critical pressure releases source windows") ||
        !require(after_critical_trim.source_scratch_capacity_bytes == 0U,
                 "critical pressure releases source scratch") ||
        !require(after_critical_trim.fragment_scratch_capacity_bytes == 0U,
                 "critical pressure releases fragment scratch") ||
        !require(after_critical_trim.trim_reclaimed_bytes > reclaimed_after_background,
                 "critical trim accounts for additional checkpoint reclamation")) {
        return 1;
    }

    zevryon::massivedoc::LayoutWindowResult critical_resume;
    used_checkpoint = false;
    if (!require(
            session.layout(
                scroll_y_q8,
                800U * 256U,
                viewport_height_q8,
                overscan_q8,
                256U,
                &critical_resume,
                &used_checkpoint,
                &error),
            error) ||
        !require(used_checkpoint, "critical-trimmed tab remains usable") ||
        !require(critical_resume.checkpoint_cache_misses >= 1U,
                 "critical resume reparses released checkpoint state") ||
        !require(critical_resume.source_window_cache_misses >= 1U,
                 "critical resume reloads bounded source window") ||
        !require(!critical_resume.fragments.empty(),
                 "critical trim preserves page correctness after resume")) {
        return 1;
    }

    if (!require(session.move_logical_record(0U, 1U, &error), error)) {
        return 1;
    }
    zevryon::massivedoc::LayoutWindowResult moved;
    used_checkpoint = false;
    if (!require(
            session.layout(
                0U,
                800U * 256U,
                viewport_height_q8,
                0U,
                256U,
                &moved,
                &used_checkpoint,
                &error),
            error) ||
        !require(used_checkpoint, "moved top query remains checkpoint accelerated") ||
        !require(!moved.fragments.empty(), "moved top query returns fragments") ||
        !require(moved.checkpoint_cache_misses >= 1U,
                 "moved physical record has an independent checkpoint cache key") ||
        !require(moved.source_window_cache_misses >= 1U,
                 "moved physical record has an independent source-window cache key")) {
        return 1;
    }
    bool saw_moved_source = false;
    bool saw_moved_hard_break = false;
    for (const auto& fragment : moved.fragments) {
        if (fragment.logical_id != kSecondLogicalId) {
            continue;
        }
        saw_moved_source = true;
        saw_moved_hard_break = saw_moved_hard_break || fragment.hard_break;
        if (!require(fragment.record_index == 0U,
                     "moved checkpoint payload emits its new logical ordinal")) {
            return 1;
        }
    }
    if (!require(saw_moved_source,
                 "moved query resolves the second physical source") ||
        !require(saw_moved_hard_break,
                 "moved query keeps newline behavior of the second physical payload")) {
        return 1;
    }

    zevryon::massivedoc::LayoutWindowResult moved_repeated;
    used_checkpoint = false;
    if (!require(
            session.layout(
                0U,
                800U * 256U,
                viewport_height_q8,
                0U,
                256U,
                &moved_repeated,
                &used_checkpoint,
                &error),
            error) ||
        !require(used_checkpoint, "repeated moved query remains accelerated") ||
        !require(moved_repeated.source_bytes_read == 0U,
                 "repeated moved query reuses physical source window") ||
        !require(moved_repeated.checkpoint_cache_hits >= 2U,
                 "repeated moved query reuses physical checkpoint") ||
        !require(moved_repeated.source_window_cache_hits >= 1U,
                 "repeated moved query reuses physical source cache")) {
        return 1;
    }
    if (!require(session.move_logical_record(1U, 0U, &error), error)) {
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
