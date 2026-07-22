#include "zenith_layout_window.hpp"

#include "compact_document.hpp"
#include "layout_checkpoint.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace zevryon::massivedoc {
namespace {

struct ScrollAnchor {
    bool present{false};
    std::uint64_t record_index{0};
    std::uint64_t old_y_q8{0};
    std::uint64_t old_height_q8{0};
    std::uint64_t local_q8{0};
    std::uint64_t new_y_q8{0};
    std::uint64_t new_height_q8{0};
};

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

bool intersects(
    std::uint64_t start,
    std::uint64_t height,
    std::uint64_t query_start,
    std::uint64_t query_end) noexcept {
    return saturating_add(start, height) > query_start && start < query_end;
}

std::uint32_t bucket_width(std::uint32_t width_q8, std::uint32_t bucket_q8) noexcept {
    if (bucket_q8 == 0U || width_q8 < bucket_q8) {
        return width_q8;
    }
    return static_cast<std::uint32_t>((width_q8 / bucket_q8) * bucket_q8);
}

LayoutCheckpointConfig checkpoint_config_for(
    const LayoutConfig& config,
    std::uint32_t viewport_width_q8) noexcept {
    LayoutCheckpointConfig checkpoint;
    checkpoint.width_q8 = bucket_width(viewport_width_q8, config.width_bucket_q8);
    checkpoint.average_advance_q8 = config.average_advance_q8;
    checkpoint.line_height_q8 = config.line_height_q8;
    checkpoint.horizontal_padding_q8 = config.horizontal_padding_q8;
    checkpoint.vertical_padding_q8 = config.vertical_padding_q8;
    checkpoint.target_stride_bytes = config.checkpoint_stride_bytes;
    return checkpoint;
}

ScrollAnchor select_scroll_anchor(
    const ViewportResult& viewport,
    std::uint64_t scroll_y_q8) noexcept {
    ScrollAnchor anchor;
    for (const MaterializedRecord& record : viewport.records) {
        const std::uint64_t end_q8 = saturating_add(record.y_q8, record.height_q8);
        if (scroll_y_q8 >= record.y_q8 && scroll_y_q8 < end_q8) {
            anchor.present = true;
            anchor.record_index = record.record_index;
            anchor.old_y_q8 = record.y_q8;
            anchor.old_height_q8 = record.height_q8;
            anchor.local_q8 = scroll_y_q8 - record.y_q8;
            anchor.new_y_q8 = record.y_q8;
            anchor.new_height_q8 = record.height_q8;
            return anchor;
        }
    }
    if (viewport.records.empty()) {
        return anchor;
    }
    const MaterializedRecord& fallback = scroll_y_q8 < viewport.records.front().y_q8
                                             ? viewport.records.front()
                                             : viewport.records.back();
    anchor.present = true;
    anchor.record_index = fallback.record_index;
    anchor.old_y_q8 = fallback.y_q8;
    anchor.old_height_q8 = fallback.height_q8;
    anchor.local_q8 = scroll_y_q8 > fallback.y_q8 ? scroll_y_q8 - fallback.y_q8 : 0U;
    if (anchor.old_height_q8 != 0U && anchor.local_q8 >= anchor.old_height_q8) {
        anchor.local_q8 = anchor.old_height_q8 - 1U;
    }
    anchor.new_y_q8 = fallback.y_q8;
    anchor.new_height_q8 = fallback.height_q8;
    return anchor;
}

void apply_height_to_anchor(
    ScrollAnchor* anchor,
    const MaterializedRecord& record,
    std::uint32_t measured_height_q8) noexcept {
    if (anchor == nullptr || !anchor->present) {
        return;
    }
    const std::uint64_t old_height_q8 = record.height_q8;
    const std::uint64_t new_height_q8 = measured_height_q8;
    if (record.record_index < anchor->record_index) {
        if (new_height_q8 >= old_height_q8) {
            anchor->new_y_q8 = saturating_add(
                anchor->new_y_q8, new_height_q8 - old_height_q8);
        } else {
            const std::uint64_t delta = old_height_q8 - new_height_q8;
            anchor->new_y_q8 = anchor->new_y_q8 >= delta
                                   ? anchor->new_y_q8 - delta
                                   : 0U;
        }
    } else if (record.record_index == anchor->record_index) {
        anchor->new_height_q8 = new_height_q8;
    }
}

std::uint64_t anchored_scroll(
    const ScrollAnchor& anchor,
    std::uint64_t original_scroll_q8) noexcept {
    if (!anchor.present || anchor.old_height_q8 == 0U || anchor.new_height_q8 == 0U) {
        return original_scroll_q8;
    }
    const std::uint64_t scaled_local_q8 =
        (anchor.local_q8 * anchor.new_height_q8) / anchor.old_height_q8;
    return saturating_add(
        anchor.new_y_q8,
        std::min(scaled_local_q8, anchor.new_height_q8 - 1U));
}

bool open_checkpoint_if_available(
    const std::filesystem::path& store_root,
    const MaterializedRecord& record,
    const LayoutConfig& config,
    const LayoutCheckpointConfig& checkpoint_config,
    LayoutCheckpointIndex* checkpoint,
    std::string* error) {
    if (!config.enable_persistent_checkpoints ||
        record.source_bytes < config.checkpoint_min_record_bytes ||
        checkpoint == nullptr || error == nullptr) {
        return false;
    }
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(
        layout_checkpoint_path(store_root, record.record_index, checkpoint_config),
        exists_error);
    if (exists_error || !exists) {
        error->clear();
        return false;
    }
    if (!checkpoint->open(
            store_root,
            record.record_index,
            record.logical_id,
            record.source_bytes,
            checkpoint_config,
            error)) {
        error->clear();
        return false;
    }
    return true;
}

} // namespace

bool layout_window_with_persistent_checkpoints(
    const std::filesystem::path& store_root,
    LayoutConfig config,
    std::uint64_t scroll_y_q8,
    std::uint32_t viewport_width_q8,
    std::uint64_t viewport_height_q8,
    std::uint64_t overscan_q8,
    std::size_t max_fragments,
    LayoutWindowResult* result,
    bool* used_checkpoint_path,
    std::string* error) {
    if (result == nullptr || used_checkpoint_path == nullptr || error == nullptr ||
        viewport_width_q8 == 0U || viewport_height_q8 == 0U || max_fragments == 0U ||
        config.average_advance_q8 == 0U || config.line_height_q8 == 0U ||
        config.width_bucket_q8 == 0U || config.checkpoint_stride_bytes == 0U) {
        if (error != nullptr) {
            *error = "invalid checkpoint-aware layout request";
        }
        return false;
    }
    *used_checkpoint_path = false;
    *result = LayoutWindowResult{};
    error->clear();

    CompactArenaReader arena(store_root);
    if (!arena.open(error)) {
        return false;
    }
    ViewportResult initial;
    if (!arena.materialize(
            scroll_y_q8,
            viewport_height_q8,
            overscan_q8,
            max_fragments,
            &initial,
            error)) {
        return false;
    }
    if (initial.records.empty()) {
        error->clear();
        return true;
    }

    result->query_start_q8 = initial.query_start_q8;
    result->query_end_q8 = initial.query_end_q8;
    result->truncated = initial.truncated;
    ScrollAnchor anchor = select_scroll_anchor(initial, scroll_y_q8);
    const LayoutCheckpointConfig checkpoint_config =
        checkpoint_config_for(config, viewport_width_q8);
    std::unordered_set<std::uint64_t> charged_indices;

    for (const MaterializedRecord& record : initial.records) {
        LayoutCheckpointIndex checkpoint;
        if (!open_checkpoint_if_available(
                store_root,
                record,
                config,
                checkpoint_config,
                &checkpoint,
                error)) {
            *result = LayoutWindowResult{};
            return true;
        }
        const LayoutCheckpointStats& stats = checkpoint.stats();
        ++result->checkpoint_hits;
        ++result->measured_records;
        if (charged_indices.insert(record.record_index).second) {
            result->checkpoint_index_bytes += stats.physical_bytes;
        }
        result->height_saturated = result->height_saturated || stats.height_saturated;
        apply_height_to_anchor(&anchor, record, stats.measured_height_q8);
        if (record.height_q8 != stats.measured_height_q8) {
            HeightUpdateResult update;
            if (!arena.update_height(
                    record.record_index,
                    stats.measured_height_q8,
                    &update,
                    error)) {
                return false;
            }
        }
    }

    const std::uint64_t anchor_corrected_scroll = anchored_scroll(anchor, scroll_y_q8);
    result->scroll_anchor_adjusted = anchor_corrected_scroll != scroll_y_q8;
    const std::uint64_t corrected_total_height = arena.stats().total_height_q8;
    const std::uint64_t max_corrected_scroll = corrected_total_height > viewport_height_q8
                                                   ? corrected_total_height - viewport_height_q8
                                                   : 0U;
    const std::uint64_t corrected_scroll_y =
        std::min(anchor_corrected_scroll, max_corrected_scroll);
    result->scroll_clamped = corrected_scroll_y != anchor_corrected_scroll;

    ViewportResult corrected;
    if (!arena.materialize(
            corrected_scroll_y,
            viewport_height_q8,
            overscan_q8,
            max_fragments,
            &corrected,
            error)) {
        return false;
    }
    result->query_start_q8 = corrected.query_start_q8;
    result->query_end_q8 = corrected.query_end_q8;
    result->total_height_q8 = corrected.total_height_q8;
    result->truncated = result->truncated || corrected.truncated;

    for (const MaterializedRecord& record : corrected.records) {
        if (result->fragments.size() >= max_fragments) {
            result->truncated = true;
            break;
        }
        LayoutCheckpointIndex checkpoint;
        if (!open_checkpoint_if_available(
                store_root,
                record,
                config,
                checkpoint_config,
                &checkpoint,
                error)) {
            *result = LayoutWindowResult{};
            return true;
        }
        ++result->checkpoint_hits;
        if (charged_indices.insert(record.record_index).second) {
            result->checkpoint_index_bytes += checkpoint.stats().physical_bytes;
        }
        const std::uint64_t local_start = corrected.query_start_q8 > record.y_q8
                                              ? corrected.query_start_q8 - record.y_q8
                                              : 0U;
        const std::uint64_t local_end = corrected.query_end_q8 > record.y_q8
                                            ? corrected.query_end_q8 - record.y_q8
                                            : 0U;
        const std::size_t remaining = max_fragments - result->fragments.size();
        std::vector<LayoutFragment> local_fragments;
        std::uint64_t source_bytes_read = 0U;
        std::uint64_t checkpoint_source_offset = 0U;
        bool truncated = false;
        if (!scan_layout_window_from_checkpoint(
                store_root,
                record,
                checkpoint,
                local_start,
                local_end,
                remaining,
                &local_fragments,
                &source_bytes_read,
                &checkpoint_source_offset,
                &truncated,
                error)) {
            return false;
        }
        static_cast<void>(checkpoint_source_offset);
        result->source_bytes_read += source_bytes_read;
        result->truncated = result->truncated || truncated;
        for (const LayoutFragment& local : local_fragments) {
            const std::uint64_t absolute_y = saturating_add(record.y_q8, local.y_q8);
            if (!intersects(
                    absolute_y,
                    local.height_q8,
                    corrected.query_start_q8,
                    corrected.query_end_q8)) {
                continue;
            }
            if (result->fragments.size() >= max_fragments) {
                result->truncated = true;
                break;
            }
            LayoutFragment absolute = local;
            absolute.y_q8 = absolute_y;
            result->fragments.push_back(absolute);
        }
    }

    result->checkpoint_accelerated = true;
    *used_checkpoint_path = true;
    return true;
}

std::string zenith_layout_window_json(const LayoutWindowResult& result) {
    std::ostringstream output;
    output << "{\"query_start_q8\":" << result.query_start_q8
           << ",\"query_end_q8\":" << result.query_end_q8
           << ",\"total_height_q8\":" << result.total_height_q8
           << ",\"source_bytes_read\":" << result.source_bytes_read
           << ",\"measured_records\":" << result.measured_records
           << ",\"cache_hits\":" << result.cache_hits
           << ",\"cache_misses\":" << result.cache_misses
           << ",\"checkpoint_hits\":" << result.checkpoint_hits
           << ",\"checkpoint_index_bytes\":" << result.checkpoint_index_bytes
           << ",\"cache_bytes\":" << result.cache_bytes
           << ",\"truncated\":" << (result.truncated ? "true" : "false")
           << ",\"height_saturated\":"
           << (result.height_saturated ? "true" : "false")
           << ",\"scroll_anchor_adjusted\":"
           << (result.scroll_anchor_adjusted ? "true" : "false")
           << ",\"scroll_clamped\":" << (result.scroll_clamped ? "true" : "false")
           << ",\"checkpoint_accelerated\":"
           << (result.checkpoint_accelerated ? "true" : "false")
           << ",\"fragment_count\":" << result.fragments.size()
           << ",\"fragments\":[";
    bool first = true;
    for (const LayoutFragment& fragment : result.fragments) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << "{\"record_index\":" << fragment.record_index
               << ",\"logical_id\":" << fragment.logical_id
               << ",\"source_start\":" << fragment.source_start
               << ",\"source_end\":" << fragment.source_end
               << ",\"x_q8\":" << fragment.x_q8
               << ",\"y_q8\":" << fragment.y_q8
               << ",\"width_q8\":" << fragment.width_q8
               << ",\"height_q8\":" << fragment.height_q8
               << ",\"hard_break\":" << (fragment.hard_break ? "true" : "false")
               << '}';
    }
    output << "]}";
    return output.str();
}

} // namespace zevryon::massivedoc
