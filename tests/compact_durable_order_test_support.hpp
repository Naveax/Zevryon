#pragma once

#include "compact_document.hpp"
#include "logical_order_persistence.hpp"
#include "massivedoc_store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zevryon_test {
namespace {

std::vector<std::byte> durable_order_bytes(std::string_view text) {
    std::vector<std::byte> output;
    output.reserve(text.size());
    for (const char value : text) {
        output.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    return output;
}

} // namespace

inline bool run_compact_durable_order_tests() {
    using zevryon::massivedoc::ArenaConfig;
    using zevryon::massivedoc::ArenaStats;
    using zevryon::massivedoc::CompactArenaReader;
    using zevryon::massivedoc::CorpusMetadata;
    using zevryon::massivedoc::HeightUpdateResult;
    using zevryon::massivedoc::SequencePosition;
    using zevryon::massivedoc::StoreStats;
    using zevryon::massivedoc::StoreWriter;
    using zevryon::massivedoc::build_compact_arena;
    using zevryon::massivedoc::logical_order_generation_path;
    using zevryon::massivedoc::logical_order_generation_temp_path;
    using zevryon::massivedoc::publish_logical_order_snapshot;

    const auto require = [](bool condition, const std::string& message) {
        if (!condition) {
            std::cerr << "FAILED: compact durable order: " << message << '\n';
            return false;
        }
        return true;
    };

    const auto root = std::filesystem::temp_directory_path() /
                      "zevryon-compact-durable-order-tests";
    std::error_code fs_error;
    std::filesystem::remove_all(root, fs_error);
    fs_error.clear();

    StoreWriter writer(root, {.segment_bytes = 4096U, .records_per_search_block = 16U});
    const auto first = durable_order_bytes("source-zero\nline-two");
    const auto second = durable_order_bytes("source-one");
    const auto third = durable_order_bytes("source-two-with-more-bytes");
    std::string error;
    if (!require(writer.append(100U, first, &error), error) ||
        !require(writer.append(101U, second, &error), error) ||
        !require(writer.append(102U, third, &error), error)) {
        return false;
    }

    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = static_cast<std::uint64_t>(
        first.size() + second.size() + third.size());
    metadata.logical_records = 3U;
    metadata.logical_nodes = 24U;
    metadata.style_runs = 12U;
    metadata.resource_references = 1U;
    metadata.largest_record_bytes = static_cast<std::uint64_t>(
        std::max({first.size(), second.size(), third.size()}));
    StoreStats store_stats;
    if (!require(writer.finalize(metadata, &store_stats, &error), error)) {
        return false;
    }

    ArenaConfig config;
    config.records_per_block = 1U;
    config.estimated_bytes_per_line = 8U;
    config.line_height_q8 = 16U * 256U;
    config.vertical_padding_q8 = 8U * 256U;
    ArenaStats arena_stats;
    if (!require(build_compact_arena(root, config, &arena_stats, &error), error)) {
        return false;
    }

    CompactArenaReader reader(root);
    if (!require(reader.open(&error), error)) {
        return false;
    }
    const auto frozen = reader.logical_snapshot();
    SequencePosition initial0;
    SequencePosition initial1;
    SequencePosition initial2;
    if (!require(frozen.at(0U, &initial0, &error), error) ||
        !require(frozen.at(1U, &initial1, &error), error) ||
        !require(frozen.at(2U, &initial2, &error), error) ||
        !require(initial0.record.source_record_index == 0U, "initial source 0") ||
        !require(initial1.record.source_record_index == 1U, "initial source 1") ||
        !require(initial2.record.source_record_index == 2U, "initial source 2")) {
        return false;
    }

    if (!require(reader.move_logical_record(0U, 2U, &error), error) ||
        !require(std::filesystem::exists(logical_order_generation_path(root, 1U)),
                 "first move publishes generation one")) {
        return false;
    }
    const auto moved = reader.logical_snapshot();
    SequencePosition moved0;
    SequencePosition moved1;
    SequencePosition moved2;
    if (!require(moved.at(0U, &moved0, &error), error) ||
        !require(moved.at(1U, &moved1, &error), error) ||
        !require(moved.at(2U, &moved2, &error), error) ||
        !require(moved0.record.source_record_index == 1U, "source 1 moves to logical 0") ||
        !require(moved1.record.source_record_index == 2U, "source 2 moves to logical 1") ||
        !require(moved2.record.source_record_index == 0U, "source 0 moves to logical 2") ||
        !require(moved2.record.logical_id == 100U, "moved record keeps logical identity")) {
        return false;
    }

    const std::uint32_t durable_height = moved2.record.height_q8 + 3U * 256U;
    HeightUpdateResult height_update;
    if (!require(reader.update_height(2U, durable_height, &height_update, &error), error) ||
        !require(height_update.block_index == 0U,
                 "moved logical record persists height to physical block zero")) {
        return false;
    }

    SequencePosition frozen_after_move;
    if (!require(frozen.at(0U, &frozen_after_move, &error), error) ||
        !require(frozen_after_move.record.source_record_index == 0U,
                 "pre-move snapshot keeps source order") ||
        !require(frozen_after_move.record.height_q8 == initial0.record.height_q8,
                 "pre-move snapshot keeps pre-update height")) {
        return false;
    }

    CompactArenaReader reopened(root);
    if (!require(reopened.open(&error), error)) {
        return false;
    }
    const auto recovered1 = reopened.logical_snapshot();
    SequencePosition recovered10;
    SequencePosition recovered11;
    SequencePosition recovered12;
    if (!require(recovered1.at(0U, &recovered10, &error), error) ||
        !require(recovered1.at(1U, &recovered11, &error), error) ||
        !require(recovered1.at(2U, &recovered12, &error), error) ||
        !require(recovered10.record.source_record_index == 1U, "reopen keeps source 1 at logical 0") ||
        !require(recovered11.record.source_record_index == 2U, "reopen keeps source 2 at logical 1") ||
        !require(recovered12.record.source_record_index == 0U, "reopen keeps source 0 at logical 2") ||
        !require(recovered12.record.height_q8 == durable_height,
                 "reopen combines durable order with physical height update")) {
        return false;
    }

    if (!require(reopened.move_logical_record(2U, 1U, &error), error) ||
        !require(std::filesystem::exists(logical_order_generation_path(root, 2U)),
                 "second move publishes generation two") ||
        !require(reopened.move_logical_record(1U, 1U, &error), error) ||
        !require(!std::filesystem::exists(logical_order_generation_path(root, 3U)),
                 "no-op move does not publish a generation")) {
        return false;
    }

    CompactArenaReader recovered_twice(root);
    if (!require(recovered_twice.open(&error), error)) {
        return false;
    }
    const auto recovered2 = recovered_twice.logical_snapshot();
    SequencePosition recovered20;
    SequencePosition recovered21;
    SequencePosition recovered22;
    if (!require(recovered2.at(0U, &recovered20, &error), error) ||
        !require(recovered2.at(1U, &recovered21, &error), error) ||
        !require(recovered2.at(2U, &recovered22, &error), error) ||
        !require(recovered20.record.source_record_index == 1U, "generation two source 1 at logical 0") ||
        !require(recovered21.record.source_record_index == 0U, "generation two source 0 at logical 1") ||
        !require(recovered22.record.source_record_index == 2U, "generation two source 2 at logical 2") ||
        !require(recovered21.record.height_q8 == durable_height,
                 "generation two keeps source-zero physical height")) {
        return false;
    }

    const auto torn_temp = logical_order_generation_temp_path(root, 3U);
    {
        std::ofstream stream(torn_temp, std::ios::binary | std::ios::trunc);
        stream << "torn";
    }
    CompactArenaReader ignores_torn_temp(root);
    if (!require(ignores_torn_temp.open(&error), error)) {
        return false;
    }
    SequencePosition torn_position;
    if (!require(ignores_torn_temp.logical_snapshot().at(1U, &torn_position, &error), error) ||
        !require(torn_position.record.source_record_index == 0U,
                 "torn higher temporary generation is ignored")) {
        return false;
    }

    const std::vector<std::uint64_t> generation3{1U, 0U, 2U};
    if (!require(publish_logical_order_snapshot(root, 3U, generation3, &error), error)) {
        return false;
    }
    const auto generation3_path = logical_order_generation_path(root, 3U);
    const std::uint64_t generation3_size = std::filesystem::file_size(generation3_path, fs_error);
    if (!require(!fs_error && generation3_size != 0U, "generation three has bytes")) {
        return false;
    }
    {
        std::fstream stream(generation3_path, std::ios::binary | std::ios::in | std::ios::out);
        if (!require(static_cast<bool>(stream), "open committed generation for corruption oracle")) {
            return false;
        }
        stream.seekg(static_cast<std::streamoff>(generation3_size - 1U), std::ios::beg);
        char value = 0;
        stream.read(&value, 1);
        value ^= 0x01;
        stream.clear();
        stream.seekp(static_cast<std::streamoff>(generation3_size - 1U), std::ios::beg);
        stream.write(&value, 1);
        stream.flush();
        if (!require(static_cast<bool>(stream), "corrupt committed generation CRC")) {
            return false;
        }
    }
    CompactArenaReader rejects_corrupt_commit(root);
    if (!require(!rejects_corrupt_commit.open(&error),
                 "corrupt committed higher generation fails arena open closed")) {
        return false;
    }

    std::filesystem::remove_all(root, fs_error);
    return require(!fs_error, "durable order fixture cleanup");
}

} // namespace zevryon_test
