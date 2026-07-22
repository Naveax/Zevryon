#include "layout_checkpoint.hpp"
#include "massivedoc_store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

} // namespace

int main() {
    const auto root =
        std::filesystem::temp_directory_path() / "zevryon-layout-checkpoint-tests";
    std::error_code fs_error;
    std::filesystem::remove_all(root, fs_error);
    fs_error.clear();

    constexpr std::uint64_t kRecordBytes = 8ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t kLogicalId = 77U;
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
        !require(generated == kRecordBytes, "checkpoint fixture generated completely")) {
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

    zevryon::massivedoc::LayoutCheckpointConfig config;
    config.width_q8 = 800U * 256U;
    config.target_stride_bytes = 64U * 1024U;
    zevryon::massivedoc::LayoutCheckpointStats built;
    if (!require(
            zevryon::massivedoc::build_layout_checkpoint(
                root, 0U, config, &built, &error),
            error) ||
        !require(built.source_bytes == kRecordBytes, "checkpoint records source length") ||
        !require(built.logical_id == kLogicalId, "checkpoint records logical identity") ||
        !require(built.entry_count > 100U, "checkpoint creates sparse entries") ||
        !require(
            built.physical_bytes < kRecordBytes / 100U,
            "checkpoint overhead remains below one percent")) {
        return 1;
    }

    zevryon::massivedoc::LayoutCheckpointIndex index;
    if (!require(
            index.open(root, 0U, kLogicalId, kRecordBytes, config, &error),
            error) ||
        !require(
            index.entries().size() == static_cast<std::size_t>(built.entry_count),
            "checkpoint entry count roundtrips")) {
        return 1;
    }

    zevryon::massivedoc::MaterializedRecord record;
    record.record_index = 0U;
    record.logical_id = kLogicalId;
    record.y_q8 = 0U;
    record.height_q8 = built.measured_height_q8;
    record.source_bytes = kRecordBytes;
    const std::uint64_t visible_start =
        static_cast<std::uint64_t>(built.measured_height_q8) / 2U;
    const std::uint64_t visible_end = visible_start + 1080U * 256U;

    std::vector<zevryon::massivedoc::LayoutFragment> fragments;
    std::uint64_t source_bytes_read = 0U;
    std::uint64_t checkpoint_source_offset = 0U;
    bool truncated = false;
    if (!require(
            zevryon::massivedoc::scan_layout_window_from_checkpoint(
                root,
                record,
                index,
                visible_start,
                visible_end,
                256U,
                &fragments,
                &source_bytes_read,
                &checkpoint_source_offset,
                &truncated,
                &error),
            error) ||
        !require(!fragments.empty(), "checkpoint scan materializes visible fragments") ||
        !require(!truncated, "checkpoint fixture does not truncate") ||
        !require(checkpoint_source_offset > 0U, "checkpoint scan seeks inside record") ||
        !require(
            source_bytes_read <= 2U * 64U * 1024U,
            "checkpoint scan reads at most two I/O windows") ||
        !require(
            source_bytes_read < kRecordBytes / 16U,
            "checkpoint scan avoids full-record read")) {
        return 1;
    }
    for (const auto& fragment : fragments) {
        if (!require(fragment.record_index == 0U, "checkpoint fragment identity") ||
            !require(fragment.source_end > fragment.source_start, "checkpoint fragment advances") ||
            !require(fragment.source_end <= kRecordBytes, "checkpoint fragment remains in record")) {
            return 1;
        }
    }

    const auto checkpoint_path =
        zevryon::massivedoc::layout_checkpoint_path(root, 0U, config);
    std::fstream corrupt(
        checkpoint_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!require(static_cast<bool>(corrupt), "open checkpoint for corruption test")) {
        return 1;
    }
    corrupt.seekg(-1, std::ios::end);
    char byte = '\0';
    corrupt.read(&byte, 1);
    corrupt.seekp(-1, std::ios::end);
    corrupt.put(static_cast<char>(byte ^ 0x5a));
    corrupt.close();

    zevryon::massivedoc::LayoutCheckpointIndex corrupted;
    error.clear();
    if (!require(
            !corrupted.open(root, 0U, kLogicalId, kRecordBytes, config, &error),
            "corrupted checkpoint must be rejected") ||
        !require(
            error.find("checksum") != std::string::npos,
            "checkpoint corruption failure is diagnostic")) {
        return 1;
    }

    std::filesystem::remove_all(root, fs_error);
    if (!require(!fs_error, "checkpoint test cleanup")) {
        return 1;
    }
    std::cout << "layout checkpoint tests passed\n";
    return 0;
}
