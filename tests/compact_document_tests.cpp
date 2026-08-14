#include "compact_document.hpp"
#include "massivedoc_store.hpp"
#include "order_statistics_sequence_test_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

bool require(bool condition, const std::string& message) {
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
    if (!zevryon_test::run_order_statistics_sequence_tests()) {
        return 1;
    }

    std::string error;
    zevryon::massivedoc::ChunkedOrderStatisticsSequence source_identity_sequence(8U);
    if (!require(
            source_identity_sequence.insert(
                0U,
                zevryon::massivedoc::SequenceRecord{11U, 101U, 256U, 0x1U, 7001U},
                &error),
            error) ||
        !require(
            source_identity_sequence.insert(
                1U,
                zevryon::massivedoc::SequenceRecord{12U, 102U, 512U, 0x2U, 7002U},
                &error),
            error) ||
        !require(
            source_identity_sequence.insert(
                2U,
                zevryon::massivedoc::SequenceRecord{13U, 103U, 768U, 0x4U, 7003U},
                &error),
            error)) {
        return 1;
    }
    const auto source_identity_snapshot = source_identity_sequence.snapshot();
    if (!require(source_identity_sequence.move(0U, 2U, &error), error)) {
        return 1;
    }
    zevryon::massivedoc::SequencePosition moved_position;
    zevryon::massivedoc::SequencePosition frozen_position;
    if (!require(source_identity_sequence.at(2U, &moved_position, &error), error) ||
        !require(moved_position.record.logical_id == 11U, "moved record keeps logical identity") ||
        !require(moved_position.record.source_record_index == 7001U,
                 "moved record keeps immutable source locator") ||
        !require(source_identity_snapshot.at(0U, &frozen_position, &error), error) ||
        !require(frozen_position.record.source_record_index == 7001U,
                 "snapshot keeps original source locator")) {
        return 1;
    }
    zevryon::massivedoc::SequenceRecord erased_source_record;
    if (!require(source_identity_sequence.erase(2U, &erased_source_record, &error), error) ||
        !require(erased_source_record.source_record_index == 7001U,
                 "erase returns immutable source locator")) {
        return 1;
    }

    const auto root = std::filesystem::temp_directory_path() / "zevryon-compact-document-tests";
    std::error_code error_code;
    std::filesystem::remove_all(root, error_code);

    zevryon::massivedoc::StoreWriter writer(root, {.segment_bytes = 4096U, .records_per_search_block = 64U});
    constexpr std::uint64_t kRecords = 1000U;
    std::uint64_t payload_bytes = 0;
    for (std::uint64_t index = 0; index < kRecords; ++index) {
        const std::size_t size = static_cast<std::size_t>(32U + (index % 17U) * 113U);
        auto bytes = payload(size, static_cast<char>('a' + static_cast<char>(index % 26U)));
        payload_bytes += bytes.size();
        if (!require(writer.append(index + 1000U, bytes, &error), error)) {
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
    if (!require(writer.finalize(metadata, &store_stats, &error), error)) {
        return 1;
    }

    zevryon::massivedoc::ArenaConfig config;
    config.records_per_block = 64U;
    config.estimated_bytes_per_line = 80U;
    config.line_height_q8 = 16U * 256U;
    config.vertical_padding_q8 = 8U * 256U;
    zevryon::massivedoc::ArenaStats arena_stats;
    if (!require(zevryon::massivedoc::build_compact_arena(root, config, &arena_stats, &error), error)) {
        return 1;
    }
    if (!require(arena_stats.logical_records == kRecords, "arena record count") ||
        !require(arena_stats.logical_nodes == kRecords * 8U, "arena node count") ||
        !require(arena_stats.block_count == 16U, "arena block count") ||
        !require(arena_stats.physical_bytes == 72U + kRecords * 4U + 16U * 8U, "compact arena size")) {
        return 1;
    }

    zevryon::massivedoc::CompactArenaReader arena(root);
    if (!require(arena.open(&error), error)) {
        return 1;
    }
    const auto initial_root = arena.logical_snapshot();
    if (!require(initial_root.stats().aggregate.record_count == kRecords, "logical root record count") ||
        !require(initial_root.stats().aggregate.layout_height_q8 == arena.stats().total_height_q8,
                 "logical root height aggregate")) {
        return 1;
    }
    zevryon::massivedoc::SequencePosition first_position;
    if (!require(initial_root.at(0U, &first_position, &error), error) ||
        !require(first_position.record.logical_id == 1000U, "logical root retains first id") ||
        !require(first_position.record.text_bytes == 32U, "logical root retains first source length") ||
        !require(first_position.y_q8 == 0U, "logical root begins at y zero")) {
        return 1;
    }

    zevryon::massivedoc::ViewportResult top;
    if (!require(arena.materialize(0U, 720U * 256U, 360U * 256U, 128U, &top, &error), error) ||
        !require(!top.records.empty(), "top viewport is non-empty") ||
        !require(top.records.front().record_index == 0U, "top begins at first record") ||
        !require(top.records.size() <= 128U, "top respects materialization cap")) {
        return 1;
    }

    zevryon::massivedoc::ViewportResult middle;
    const std::uint64_t middle_y = arena.stats().total_height_q8 / 2U;
    if (!require(arena.materialize(middle_y, 720U * 256U, 360U * 256U, 64U, &middle, &error), error) ||
        !require(!middle.records.empty(), "middle viewport is non-empty") ||
        !require(middle.records.front().record_index > 0U, "middle does not scan from first record") ||
        !require(middle.records.size() <= 64U, "middle respects materialization cap")) {
        return 1;
    }
    zevryon::massivedoc::SequencePosition middle_position;
    if (!require(initial_root.locate_height_offset(middle.query_start_q8, &middle_position, &error), error) ||
        !require(middle_position.record_index == middle.records.front().record_index,
                 "viewport first record comes from sequence height select") ||
        !require(middle_position.y_q8 == middle.records.front().y_q8,
                 "viewport y comes from sequence prefix aggregate")) {
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
    if (!require(arena.materialize(end_y, 720U * 256U, 0U, 256U, &end, &error), error) ||
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
        if (!require(arena.update_height(record, new_height, &update, &error), error) ||
            !require(update.record_index == record, "height update record identity") ||
            !require(update.new_height_q8 == new_height, "height update value")) {
            return 1;
        }
        expected_total = expected_total - update.old_height_q8 + update.new_height_q8;
        if (!require(update.total_height_q8 == expected_total, "height update total invariant") ||
            !require(arena.stats().total_height_q8 == expected_total, "reader total follows update") ||
            !require(arena.logical_snapshot().stats().aggregate.layout_height_q8 == expected_total,
                     "logical root total follows update")) {
            return 1;
        }
    }
    if (!require(initial_root.stats().aggregate.layout_height_q8 == arena_stats.total_height_q8,
                 "pre-update snapshot remains isolated") ||
        !require(arena.logical_snapshot().stats().aggregate.layout_height_q8 == expected_total,
                 "live logical root diverges from old snapshot")) {
        return 1;
    }

    zevryon::massivedoc::HeightUpdateResult invalid_update;
    if (!require(!arena.update_height(kRecords, 256U, &invalid_update, &error), "out-of-range update rejected") ||
        !require(!arena.update_height(0U, 0U, &invalid_update, &error), "zero-height update rejected")) {
        return 1;
    }

    zevryon::massivedoc::CompactArenaReader reopened(root);
    if (!require(reopened.open(&error), error) ||
        !require(reopened.stats().total_height_q8 == expected_total, "height updates persist after reopen") ||
        !require(reopened.logical_snapshot().stats().aggregate.layout_height_q8 == expected_total,
                 "reopened logical root matches persisted height total")) {
        return 1;
    }
    zevryon::massivedoc::HeightUpdateResult first_update;
    constexpr std::uint32_t kFirstHeight = 400U * 256U;
    if (!require(reopened.update_height(0U, kFirstHeight, &first_update, &error), error)) {
        return 1;
    }
    zevryon::massivedoc::ViewportResult updated_top;
    if (!require(reopened.materialize(0U, 720U * 256U, 0U, 32U, &updated_top, &error), error) ||
        !require(!updated_top.records.empty(), "updated top viewport is non-empty") ||
        !require(updated_top.records.front().height_q8 == kFirstHeight, "viewport uses persisted height update") ||
        !require(reopened.logical_snapshot().stats().aggregate.layout_height_q8 == first_update.total_height_q8,
                 "updated viewport root publishes corrected total")) {
        return 1;
    }

    error_code.clear();
    std::filesystem::remove_all(root, error_code);
    if (!require(!error_code, "compact test cleanup failed: " + error_code.message())) {
        return 1;
    }
    std::cout << "compact document tests passed\n";
    return 0;
}
