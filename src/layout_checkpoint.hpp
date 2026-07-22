#pragma once

#include "layout_window.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace zevryon::massivedoc {

struct LayoutCheckpointConfig {
    std::uint32_t width_q8{800U * 256U};
    std::uint32_t average_advance_q8{8U * 256U};
    std::uint32_t line_height_q8{18U * 256U};
    std::uint32_t horizontal_padding_q8{12U * 256U};
    std::uint32_t vertical_padding_q8{12U * 256U};
    std::uint32_t target_stride_bytes{256U * 1024U};
};

struct LayoutCheckpointEntry {
    std::uint64_t source_offset{0};
    std::uint64_t line_index{0};
    std::uint64_t y_q8{0};
};

struct LayoutCheckpointStats {
    std::uint64_t record_index{0};
    std::uint64_t logical_id{0};
    std::uint64_t source_bytes{0};
    std::uint32_t measured_height_q8{0};
    bool height_saturated{false};
    std::uint64_t entry_count{0};
    std::uint64_t physical_bytes{0};
    LayoutCheckpointConfig config{};
};

std::filesystem::path layout_checkpoint_path(
    const std::filesystem::path& store_root,
    std::uint64_t record_index,
    const LayoutCheckpointConfig& config);

bool build_layout_checkpoint(
    const std::filesystem::path& store_root,
    std::uint64_t record_index,
    LayoutCheckpointConfig config,
    LayoutCheckpointStats* stats,
    std::string* error);

class LayoutCheckpointIndex {
public:
    bool open(
        const std::filesystem::path& store_root,
        std::uint64_t record_index,
        std::uint64_t logical_id,
        std::uint64_t source_bytes,
        LayoutCheckpointConfig config,
        std::string* error);

    const LayoutCheckpointStats& stats() const noexcept;
    const std::vector<LayoutCheckpointEntry>& entries() const noexcept;
    const LayoutCheckpointEntry& entry_for_y(std::uint64_t local_y_q8) const noexcept;

private:
    LayoutCheckpointStats stats_{};
    std::vector<LayoutCheckpointEntry> entries_;
    bool opened_{false};
};

bool scan_layout_window_from_checkpoint(
    const std::filesystem::path& store_root,
    const MaterializedRecord& record,
    const LayoutCheckpointIndex& checkpoint,
    std::uint64_t visible_start_q8,
    std::uint64_t visible_end_q8,
    std::size_t max_fragments,
    std::vector<LayoutFragment>* fragments,
    std::uint64_t* source_bytes_read,
    std::uint64_t* checkpoint_source_offset,
    bool* truncated,
    std::string* error);

std::string layout_checkpoint_stats_json(const LayoutCheckpointStats& stats);

} // namespace zevryon::massivedoc
