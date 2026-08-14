#pragma once

#include "compact_document.hpp"
#include "layout_checkpoint.hpp"
#include "layout_window.hpp"
#include "massivedoc_store.hpp"
#include "zenith_hot_scroll.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace zevryon_test {
namespace {

std::vector<std::byte> durable_consumer_payload(
    std::size_t size,
    bool with_newlines,
    char fill) {
    std::vector<std::byte> output(size);
    for (std::size_t index = 0U; index < size; ++index) {
        char value = fill;
        if (with_newlines && index % 64U == 63U) {
            value = '\n';
        }
        output[index] = static_cast<std::byte>(static_cast<unsigned char>(value));
    }
    return output;
}

bool durable_consumer_fragment_state(
    const zevryon::massivedoc::LayoutWindowResult& result,
    std::uint64_t logical_id,
    std::uint64_t expected_record_index,
    bool* saw_record,
    bool* saw_hard_break,
    std::string* error) {
    if (saw_record == nullptr || saw_hard_break == nullptr || error == nullptr) {
        return false;
    }
    *saw_record = false;
    *saw_hard_break = false;
    for (const auto& fragment : result.fragments) {
        if (fragment.logical_id != logical_id) {
            continue;
        }
        *saw_record = true;
        *saw_hard_break = *saw_hard_break || fragment.hard_break;
        if (fragment.record_index != expected_record_index) {
            *error = "consumer fragment emitted wrong logical ordinal";
            return false;
        }
    }
    return true;
}

} // namespace

inline bool run_durable_consumer_reopen_tests() {
    using zevryon::massivedoc::ArenaConfig;
    using zevryon::massivedoc::ArenaStats;
    using zevryon::massivedoc::CorpusMetadata;
    using zevryon::massivedoc::LayoutCheckpointConfig;
    using zevryon::massivedoc::LayoutCheckpointStats;
    using zevryon::massivedoc::LayoutConfig;
    using zevryon::massivedoc::LayoutWindowEngine;
    using zevryon::massivedoc::LayoutWindowResult;
    using zevryon::massivedoc::StoreStats;
    using zevryon::massivedoc::StoreWriter;
    using zevryon::massivedoc::ZenithHotScrollSession;
    using zevryon::massivedoc::build_compact_arena;
    using zevryon::massivedoc::build_layout_checkpoint;

    const auto require = [](bool condition, const std::string& message) {
        if (!condition) {
            std::cerr << "FAILED: durable consumer reopen: " << message << '\n';
            return false;
        }
        return true;
    };

    const auto root = std::filesystem::temp_directory_path() /
                      "zevryon-durable-consumer-reopen-tests";
    std::error_code fs_error;
    std::filesystem::remove_all(root, fs_error);
    fs_error.clear();

    constexpr std::uint64_t kSource0LogicalId = 4000U;
    constexpr std::uint64_t kSource1LogicalId = 4001U;
    const auto source0 = durable_consumer_payload(4096U, false, 'a');
    const auto source1 = durable_consumer_payload(4096U, true, 'b');

    StoreWriter writer(root, {.segment_bytes = 4096U, .records_per_search_block = 16U});
    std::string error;
    if (!require(writer.append(kSource0LogicalId, source0, &error), error) ||
        !require(writer.append(kSource1LogicalId, source1, &error), error)) {
        return false;
    }
    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = static_cast<std::uint64_t>(source0.size()) +
                                  static_cast<std::uint64_t>(source1.size());
    metadata.logical_records = 2U;
    metadata.logical_nodes = 16U;
    metadata.style_runs = 8U;
    metadata.resource_references = 2U;
    metadata.largest_record_bytes = static_cast<std::uint64_t>(source0.size());
    StoreStats store_stats;
    if (!require(writer.finalize(metadata, &store_stats, &error), error)) {
        return false;
    }

    ArenaConfig arena_config;
    arena_config.records_per_block = 1U;
    arena_config.estimated_bytes_per_line = 64U;
    arena_config.line_height_q8 = 18U * 256U;
    arena_config.vertical_padding_q8 = 12U * 256U;
    ArenaStats arena_stats;
    if (!require(build_compact_arena(root, arena_config, &arena_stats, &error), error)) {
        return false;
    }

    LayoutConfig layout_config;
    layout_config.checkpoint_stride_bytes = 1024U;
    layout_config.checkpoint_min_record_bytes = 1U;
    layout_config.max_checkpoint_cache_bytes = 128U * 1024U;
    layout_config.max_source_window_cache_bytes = 128U * 1024U;
    LayoutCheckpointConfig checkpoint_config;
    checkpoint_config.width_q8 = 800U * 256U;
    checkpoint_config.average_advance_q8 = layout_config.average_advance_q8;
    checkpoint_config.line_height_q8 = layout_config.line_height_q8;
    checkpoint_config.horizontal_padding_q8 = layout_config.horizontal_padding_q8;
    checkpoint_config.vertical_padding_q8 = layout_config.vertical_padding_q8;
    checkpoint_config.target_stride_bytes = layout_config.checkpoint_stride_bytes;
    LayoutCheckpointStats checkpoint0;
    LayoutCheckpointStats checkpoint1;
    if (!require(build_layout_checkpoint(root, 0U, checkpoint_config, &checkpoint0, &error), error) ||
        !require(build_layout_checkpoint(root, 1U, checkpoint_config, &checkpoint1, &error), error)) {
        return false;
    }

    LayoutWindowEngine layout_writer(root, layout_config);
    if (!require(layout_writer.open(&error), error) ||
        !require(layout_writer.move_logical_record(0U, 1U, &error), error)) {
        return false;
    }

    LayoutWindowEngine layout_reopened(root, layout_config);
    LayoutWindowResult layout_result;
    if (!require(layout_reopened.open(&error), error) ||
        !require(layout_reopened.layout(
                     0U,
                     800U * 256U,
                     2000U * 256U,
                     0U,
                     512U,
                     &layout_result,
                     &error),
                 error) ||
        !require(!layout_result.fragments.empty(), "reopened layout returns fragments")) {
        return false;
    }
    bool saw_source1 = false;
    bool source1_hard_break = false;
    if (!require(durable_consumer_fragment_state(
                     layout_result,
                     kSource1LogicalId,
                     0U,
                     &saw_source1,
                     &source1_hard_break,
                     &error),
                 error) ||
        !require(saw_source1, "LayoutWindow reopen restores moved source 1") ||
        !require(source1_hard_break,
                 "LayoutWindow reopen reads newline-bearing physical source 1")) {
        return false;
    }

    ZenithHotScrollSession hot_reopened(root, layout_config);
    LayoutWindowResult hot_result;
    bool used_checkpoint = false;
    if (!require(hot_reopened.open(&error), error) ||
        !require(hot_reopened.layout(
                     0U,
                     800U * 256U,
                     2000U * 256U,
                     0U,
                     512U,
                     &hot_result,
                     &used_checkpoint,
                     &error),
                 error) ||
        !require(used_checkpoint, "HotScroll reopen uses physical checkpoint") ||
        !require(!hot_result.fragments.empty(), "HotScroll reopen returns fragments")) {
        return false;
    }
    saw_source1 = false;
    source1_hard_break = false;
    if (!require(durable_consumer_fragment_state(
                     hot_result,
                     kSource1LogicalId,
                     0U,
                     &saw_source1,
                     &source1_hard_break,
                     &error),
                 error) ||
        !require(saw_source1, "HotScroll reopen restores moved source 1") ||
        !require(source1_hard_break,
                 "HotScroll reopen reads newline-bearing physical source 1") ||
        !require(hot_reopened.move_logical_record(1U, 0U, &error), error)) {
        return false;
    }

    LayoutWindowEngine layout_after_hot(root, layout_config);
    LayoutWindowResult restored_identity;
    if (!require(layout_after_hot.open(&error), error) ||
        !require(layout_after_hot.layout(
                     0U,
                     800U * 256U,
                     2000U * 256U,
                     0U,
                     512U,
                     &restored_identity,
                     &error),
                 error) ||
        !require(!restored_identity.fragments.empty(),
                 "LayoutWindow sees HotScroll-published second generation")) {
        return false;
    }
    bool saw_source0 = false;
    bool source0_hard_break = false;
    if (!require(durable_consumer_fragment_state(
                     restored_identity,
                     kSource0LogicalId,
                     0U,
                     &saw_source0,
                     &source0_hard_break,
                     &error),
                 error) ||
        !require(saw_source0, "second generation restores source 0 to logical 0") ||
        !require(!source0_hard_break,
                 "source 0 does not inherit source 1 newline payload")) {
        return false;
    }

    std::filesystem::remove_all(root, fs_error);
    return require(!fs_error, "consumer reopen fixture cleanup");
}

} // namespace zevryon_test
