#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zevryon::massivedoc {

struct LogicalOrderSnapshot {
    bool persisted{false};
    std::uint64_t generation{0};
    std::vector<std::uint64_t> source_record_indices;
};

std::filesystem::path logical_order_generation_path(
    const std::filesystem::path& store_root,
    std::uint64_t generation);

std::filesystem::path logical_order_generation_temp_path(
    const std::filesystem::path& store_root,
    std::uint64_t generation);

bool parse_logical_order_snapshot(
    std::span<const std::byte> bytes,
    std::uint64_t expected_records,
    LogicalOrderSnapshot* snapshot,
    std::string* error);

bool load_logical_order_snapshot(
    const std::filesystem::path& store_root,
    std::uint64_t expected_records,
    LogicalOrderSnapshot* snapshot,
    std::string* error);

bool publish_logical_order_snapshot(
    const std::filesystem::path& store_root,
    std::uint64_t generation,
    std::span<const std::uint64_t> source_record_indices,
    std::string* error);

} // namespace zevryon::massivedoc
