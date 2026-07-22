#include "compact_document.hpp"
#include "massivedoc_store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

bool require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

std::vector<std::byte> payload(std::size_t size, char fill) {
    return std::vector<std::byte>(size, static_cast<std::byte>(static_cast<unsigned char>(fill)));
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "zevryon-compact-document-tests";
    std::error_code error_code;
    std::filesystem::remove_all(root, error_code);

    std::string error;
    zevryon::massivedoc::StoreWriter writer(root, {.segment_bytes = 4096U, .records_per_search_block = 64U});
    constexpr std::uint64_t kRecords = 1000U;
    std::uint64_t payload_bytes = 0;
    for (std::uint64_t index = 0; index < kRecords; ++index) {
        const std::size_t size = static_cast<std::size_t>(32U + (index % 17U) * 113U);
        auto bytes = payload(size, static_cast<char>('a' + static_cast<char>(index % 26U)));
        payload_bytes += bytes.size();
        if (!require(writer.append(index + 1000U, bytes, &error), error.c_str())) {
            return 1;
        }
    }
    zevryon::massivedoc::CorpusMetadata metadata;
    metadata.logical_utf8_bytes = payload_bytes;
    metadata.logical_records = kRecords;
    metadata.logical_nodes = kRecords * 8U;
    metadata.style_runs = kRecords * 4U;
    metadata.resource_references = kRecords / 8U;
    metadata.largest_record_bytes = 32U + 16U * 113U;
    zevryon::massivedoc::StoreStats store_stats;
    if (!require(writer.finalize(metadata, &store_stats, &error), error.c_str())) {
        return 1;
    }

    zevryon::massivedoc::ArenaConfig config;
    config.records_per_block = 64U;
    config.estimated_bytes_per_line = 80U;
    config.line_height_q8 = 16U * 256U;
    config.vertical_padding_q8 = 8U * 256U;
    zevryon::massivedoc::ArenaStats arena_stats;
    if (!require(zevryon::massivedoc::build_compact_arena(root, config, &arena_stats, &error), error.c_str())) {
        return 1;
    }
    if (!require(arena_stats.logical_records == kRecords, "arena record count") ||
        !require(arena_stats.logical_nodes == kRecords * 8U, "arena node count") ||
        !require(arena_stats.block_count == 16U, "arena block count") ||
        !require(arena_stats.physical_bytes == 72U + kRecords * 4U + 16U * 8U, "compact arena size")) {
        return 1;
    }

    zevryon::massivedoc::CompactArenaReader arena(root);
    if (!require(arena.open(&error), error.c_str())) {
        return 1;
    }
    zevryon::massivedoc::ViewportResult top;
    if (!require(arena.materialize(0U, 720U * 256U, 360U * 256U, 128U, &top, &error), error.c_str()) ||
        !require(!top.records.empty(), "top viewport is non-empty") ||
        !require(top.records.front().record_index == 0U, "top begins at first record") ||
        !require(top.records.size() <= 128U, "top respects materialization cap")) {
        return 1;
    }

    zevryon::massivedoc::ViewportResult middle;
    const std::uint64_t middle_y = arena.stats().total_height_q8 / 2U;
    if (!require(arena.materialize(middle_y, 720U * 256U, 360U * 256U, 64U, &middle, &error), error.c_str()) ||
        !require(!middle.records.empty(), "middle viewport is non-empty") ||
        !require(middle.records.front().record_index > 0U, "middle does not scan from first record") ||
        !require(middle.records.size() <= 64U, "middle respects materialization cap")) {
        return 1;
    }
    for (std::size_t index = 1; index < middle.records.size(); ++index) {
        if (!require(middle.records[index - 1U].record_index + 1U == middle.records[index].record_index,
                     "materialized records remain contiguous") ||
            !require(middle.records[index - 1U].y_q8 < middle.records[index].y_q8,
                     "materialized positions remain sorted")) {
            return 1;
        }
    }

    zevryon::massivedoc::ViewportResult end;
    const std::uint64_t end_y = arena.stats().total_height_q8 > 720U * 256U
                                    ? arena.stats().total_height_q8 - 720U * 256U
                                    : 0U;
    if (!require(arena.materialize(end_y, 720U * 256U, 0U, 256U, &end, &error), error.c_str()) ||
        !require(!end.records.empty(), "end viewport is non-empty") ||
        !require(end.records.back().record_index == kRecords - 1U, "end reaches final record") ||
        !require(end.records.back().logical_id == kRecords - 1U + 1000U, "logical id retained")) {
        return 1;
    }

    std::uint64_t expected_total = arena.stats().total_height_q8;
    for (std::uint64_t iteration = 0U; iteration < 128U; ++iteration) {
        const std::uint64_t record = (iteration * 7919U) % kRecords;
        const std::uint32_t new_height = static_cast<std::uint32_t>((24U + iteration % 97U) * 256U);
        zevryon::massivedoc::HeightUpdateResult update;
        if (!require(arena.update_height(record, new_height, &update, &error), error.c_str()) ||
            !require(update.record_index == record, "height update record identity") ||
            !require(update.new_height_q8 == new_height, "height update value")) {
            return 1;
        }
        expected_total = expected_total - update.old_height_q8 + update.new_height_q8;
        if (!require(update.total_height_q8 == expected_total, "height update total invariant") ||
            !require(arena.stats().total_height_q8 == expected_total, "reader total follows update")) {
            return 1;
        }
    }

    zevryon::massivedoc::HeightUpdateResult invalid_update;
    if (!require(!arena.update_height(kRecords, 256U, &invalid_update, &error), "out-of-range update rejected") ||
        !require(!arena.update_height(0U, 0U, &invalid_update, &error), "zero-height update rejected")) {
        return 1;
    }

    zevryon::massivedoc::CompactArenaReader reopened(root);
    if (!require(reopened.open(&error), error.c_str()) ||
        !require(reopened.stats().total_height_q8 == expected_total, "height updates persist after reopen")) {
        return 1;
    }
    zevryon::massivedoc::HeightUpdateResult first_update;
    constexpr std::uint32_t kFirstHeight = 400U * 256U;
    if (!require(reopened.update_height(0U, kFirstHeight, &first_update, &error), error.c_str())) {
        return 1;
    }
    zevryon::massivedoc::ViewportResult updated_top;
    if (!require(reopened.materialize(0U, 720U * 256U, 0U, 32U, &updated_top, &error), error.c_str()) ||
        !require(!updated_top.records.empty(), "updated top viewport is non-empty") ||
        !require(updated_top.records.front().height_q8 == kFirstHeight, "viewport uses persisted height update")) {
        return 1;
    }

    std::filesystem::remove_all(root, error_code);
    std::cout << "compact document tests passed\n";
    return 0;
}
