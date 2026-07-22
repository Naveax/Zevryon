#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace zevryon::massivedoc {

struct ArenaConfig {
    std::uint32_t records_per_block{8192U};
    std::uint32_t estimated_bytes_per_line{96U};
    std::uint32_t line_height_q8{18U * 256U};
    std::uint32_t vertical_padding_q8{12U * 256U};
};

struct ArenaStats {
    std::uint64_t logical_records{0};
    std::uint64_t logical_nodes{0};
    std::uint64_t total_height_q8{0};
    std::uint64_t block_count{0};
    std::uint64_t physical_bytes{0};
    ArenaConfig config{};
};

struct MaterializedRecord {
    std::uint64_t record_index{0};
    std::uint64_t logical_id{0};
    std::uint64_t y_q8{0};
    std::uint32_t height_q8{0};
    std::uint64_t source_bytes{0};
};

struct ViewportResult {
    std::uint64_t query_start_q8{0};
    std::uint64_t query_end_q8{0};
    std::uint64_t total_height_q8{0};
    bool truncated{false};
    std::vector<MaterializedRecord> records;
};

bool build_compact_arena(
    const std::filesystem::path& store_root,
    ArenaConfig config,
    ArenaStats* stats,
    std::string* error);

class CompactArenaReader {
public:
    explicit CompactArenaReader(const std::filesystem::path& store_root);

    bool open(std::string* error);
    const ArenaStats& stats() const noexcept;
    bool materialize(
        std::uint64_t scroll_y_q8,
        std::uint64_t viewport_height_q8,
        std::uint64_t overscan_q8,
        std::size_t max_records,
        ViewportResult* result,
        std::string* error) const;

private:
    std::filesystem::path root_;
    ArenaStats stats_{};
    std::vector<std::uint64_t> block_prefix_q8_;
    bool opened_{false};
};

std::string arena_stats_json(const ArenaStats& stats);
std::string viewport_json(const ViewportResult& result);

} // namespace zevryon::massivedoc
