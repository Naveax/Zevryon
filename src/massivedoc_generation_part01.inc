bool generation_validate_inventory(
    const std::filesystem::path& store_root,
    std::span<const GenerationSegmentInventory> segments,
    std::string* error) {
    if (segments.size() >
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        *error = "generation segment inventory exceeds supported segment identifiers";
        return false;
    }
    for (std::size_t index = 0U; index < segments.size(); ++index) {
        const auto expected_id = static_cast<std::uint32_t>(index);
        const auto& segment = segments[index];
        if (segment.segment_id != expected_id) {
            *error = "generation segment inventory is not canonical";
            return false;
        }
        std::ostringstream name;
        name << "segment-" << std::setw(8) << std::setfill('0') << segment.segment_id
             << ".bin";
        const auto path = store_root / "segments" / name.str();
        std::error_code size_error;
        const std::uint64_t actual = std::filesystem::file_size(path, size_error);
        if (size_error || actual != segment.byte_length) {
            *error = "generation segment inventory size mismatch";
            return false;
        }
    }
    return true;
}

bool generation_durable_flush_store_payload(
    const std::filesystem::path& store_root,
    std::span<const GenerationSegmentInventory> segments,
    std::string* error) {
    constexpr std::array<std::string_view, 3> kIndexNames{
        "records.idx", "chunks.idx", "search.bgm"};
    for (const std::string_view name : kIndexNames) {
        if (!generation_durable_flush_file(store_root / std::string(name), error)) {
            return false;
        }
    }
    for (const auto& segment : segments) {
        std::ostringstream name;
        name << "segment-" << std::setw(8) << std::setfill('0') << segment.segment_id
             << ".bin";
        if (!generation_durable_flush_file(store_root / "segments" / name.str(), error)) {
            return false;
        }
    }
    if (!generation_durable_sync_directory(store_root / "segments", error)) {
        return false;
    }
    return generation_durable_sync_directory(store_root, error);
}

#include "massivedoc_generation_part02.inc"
