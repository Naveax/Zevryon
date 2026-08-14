#include "massivedoc_generation.hpp"
#include "massivedoc_store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t kMib = 1024ULL * 1024ULL;
constexpr std::uint64_t kSegmentBytes = 64ULL * kMib;
constexpr std::uint32_t kSegmentCount = 64U;
constexpr std::uint64_t kLogicalBytes = kSegmentBytes * kSegmentCount;
constexpr std::uint64_t kRecordCount = kSegmentCount;
constexpr std::uint64_t kChunkCount = kSegmentCount;
constexpr std::uint64_t kSearchBlockCount = 1U;
constexpr std::uint32_t kRecordsPerSearchBlock = 8192U;
constexpr std::size_t kRecordDescriptorBytes = 32U;
constexpr std::size_t kChunkDescriptorBytes = 24U;
constexpr std::size_t kSearchSignatureBytes = 8192U;
constexpr std::size_t kManifestBytes = 160U;
constexpr std::size_t kProbeSliceBytes = 4096U;
constexpr std::array<char, 8> kManifestMagic{'Z', 'M', 'D', 'S', 'T', '0', '0', '1'};

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        output.push_back(static_cast<std::byte>(
            (value >> static_cast<unsigned int>(index * 8U)) & 0xffU));
    }
}

void append_u64(std::vector<std::byte>& output, std::uint64_t value) {
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        output.push_back(static_cast<std::byte>(
            (value >> static_cast<unsigned int>(index * 8U)) & 0xffULL));
    }
}

std::uint32_t crc32_update(std::uint32_t crc, std::span<const std::byte> bytes) {
    crc = ~crc;
    for (const std::byte raw : bytes) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<unsigned char>(raw));
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

std::filesystem::path segment_path(
    const std::filesystem::path& root,
    std::uint32_t segment_id) {
    std::ostringstream name;
    name << "segment-" << std::setw(8) << std::setfill('0') << segment_id << ".bin";
    return root / "segments" / name.str();
}

bool write_bytes(
    std::ofstream& stream,
    std::span<const std::byte> bytes,
    std::string* error,
    const char* label) {
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        *error = std::string("failed to write ") + label;
        return false;
    }
    return true;
}

std::vector<std::byte> make_manifest() {
    std::vector<std::byte> manifest;
    manifest.reserve(kManifestBytes);
    for (const char character : kManifestMagic) {
        manifest.push_back(static_cast<std::byte>(character));
    }
    append_u32(manifest, 1U);
    append_u32(manifest, static_cast<std::uint32_t>(kManifestBytes));
    append_u64(manifest, kSegmentBytes);
    append_u32(manifest, kRecordsPerSearchBlock);
    append_u32(manifest, 0U);
    append_u64(manifest, kLogicalBytes);
    append_u64(manifest, kRecordCount);
    append_u64(manifest, kRecordCount * 8U);
    append_u64(manifest, kRecordCount * 4U);
    append_u64(manifest, 8U);
    append_u64(manifest, kSegmentBytes);
    append_u64(manifest, kSegmentCount);
    append_u64(manifest, kChunkCount);
    append_u64(manifest, kSearchBlockCount);
    for (std::size_t index = 0U; index < 32U; ++index) {
        manifest.push_back(std::byte{0});
    }
    while (manifest.size() + sizeof(std::uint32_t) < kManifestBytes) {
        manifest.push_back(std::byte{0});
    }
    const std::uint32_t checksum = crc32_update(
        0U,
        std::span<const std::byte>(manifest.data(), manifest.size()));
    append_u32(manifest, checksum);
    return manifest;
}

bool prepare_fixture(const std::filesystem::path& root, std::string* error) {
    std::error_code filesystem_error;
    std::filesystem::remove_all(root, filesystem_error);
    filesystem_error.clear();
    std::filesystem::create_directories(root / "segments", filesystem_error);
    if (filesystem_error) {
        *error = "failed to create fixture directories: " + filesystem_error.message();
        return false;
    }

    std::ofstream records(root / "records.idx", std::ios::binary | std::ios::trunc);
    std::ofstream chunks(root / "chunks.idx", std::ios::binary | std::ios::trunc);
    std::ofstream search(root / "search.bgm", std::ios::binary | std::ios::trunc);
    if (!records || !chunks || !search) {
        *error = "failed to create sparse fixture indexes";
        return false;
    }

    for (std::uint32_t index = 0U; index < kSegmentCount; ++index) {
        std::vector<std::byte> record;
        record.reserve(kRecordDescriptorBytes);
        append_u64(record, index);
        append_u64(record, index);
        append_u64(record, kSegmentBytes);
        append_u32(record, 1U);
        append_u32(record, 0U);
        if (record.size() != kRecordDescriptorBytes ||
            !write_bytes(records, record, error, "record descriptor")) {
            return false;
        }

        std::vector<std::byte> chunk;
        chunk.reserve(kChunkDescriptorBytes);
        append_u32(chunk, index);
        append_u32(chunk, 0U);
        append_u64(chunk, 0U);
        append_u64(chunk, kSegmentBytes);
        if (chunk.size() != kChunkDescriptorBytes ||
            !write_bytes(chunks, chunk, error, "chunk descriptor")) {
            return false;
        }
    }

    std::array<std::byte, kSearchSignatureBytes> signature{};
    if (!write_bytes(search, signature, error, "search signature")) {
        return false;
    }
    records.close();
    chunks.close();
    search.close();
    if (records.fail() || chunks.fail() || search.fail()) {
        *error = "failed to close sparse fixture indexes";
        return false;
    }

    std::vector<zevryon::massivedoc::GenerationSegmentInventory> inventory;
    inventory.reserve(kSegmentCount);
    for (std::uint32_t segment_id = 0U; segment_id < kSegmentCount; ++segment_id) {
        const auto path = segment_path(root, segment_id);
        {
            std::ofstream segment(path, std::ios::binary | std::ios::trunc);
            if (!segment) {
                *error = "failed to create sparse fixture segment";
                return false;
            }
        }
        std::filesystem::resize_file(path, kSegmentBytes, filesystem_error);
        if (filesystem_error) {
            *error = "failed to resize sparse fixture segment: " + filesystem_error.message();
            return false;
        }
        inventory.push_back(
            zevryon::massivedoc::GenerationSegmentInventory{segment_id, kSegmentBytes});
    }

    const std::vector<std::byte> manifest = make_manifest();
    if (manifest.size() != kManifestBytes) {
        *error = "fixture manifest size mismatch";
        return false;
    }
    const std::array<std::uint8_t, 32> source_identity{};
    if (!zevryon::massivedoc::publish_store_generation(
            root,
            1U,
            std::span<const std::byte>(manifest.data(), manifest.size()),
            source_identity,
            std::span<const zevryon::massivedoc::GenerationSegmentInventory>(
                inventory.data(), inventory.size()),
            zevryon::massivedoc::GenerationPublicationCut::none,
            error)) {
        return false;
    }
    if (!zevryon::massivedoc::publish_legacy_store_manifest(
            root,
            std::span<const std::byte>(manifest.data(), manifest.size()),
            error)) {
        return false;
    }
    return true;
}

bool all_zero(std::span<const std::byte> bytes) {
    return std::all_of(
        bytes.begin(),
        bytes.end(),
        [](std::byte value) { return value == std::byte{0}; });
}

int run_open(
    const std::filesystem::path& root,
    std::uint64_t touch_bytes,
    std::uint64_t hot_mib,
    std::uint64_t warm_mib) {
    if (hot_mib > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(kMib) ||
        warm_mib > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(kMib)) {
        std::cerr << "cache MiB argument overflow\n";
        return 2;
    }
    if (touch_bytes > kSegmentBytes ||
        touch_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        std::cerr << "touch byte count is outside the supported probe range\n";
        return 2;
    }

    zevryon::massivedoc::StoreReadConfig config;
    config.io_window_bytes = zevryon::massivedoc::kIoWindowBytes;
    config.block_cache.block_bytes = zevryon::massivedoc::kDefaultImmutableBlockBytes;
    config.block_cache.hot_bytes =
        static_cast<std::size_t>(hot_mib) * static_cast<std::size_t>(kMib);
    config.block_cache.warm_bytes =
        static_cast<std::size_t>(warm_mib) * static_cast<std::size_t>(kMib);

    zevryon::massivedoc::StoreReader reader(root, config);
    std::string error;
    if (!reader.open(&error)) {
        std::cerr << "StoreReader::open failed: " << error << '\n';
        return 3;
    }
    const auto& stats = reader.stats();
    if (stats.corpus.logical_utf8_bytes != kLogicalBytes ||
        stats.corpus.logical_records != kRecordCount ||
        stats.segment_count != kSegmentCount ||
        stats.chunk_count != kChunkCount) {
        std::cerr << "opened store stats differ from 4 GiB fixture authority\n";
        return 4;
    }

    std::vector<std::byte> head;
    if (!reader.read_record_slice(0U, 0U, kProbeSliceBytes, &head, &error) ||
        head.size() != kProbeSliceBytes ||
        !all_zero(head)) {
        std::cerr << "failed to read zero head slice: " << error << '\n';
        return 5;
    }

    std::vector<std::byte> tail;
    if (!reader.read_record_slice(
            kRecordCount - 1U,
            kSegmentBytes - kProbeSliceBytes,
            kProbeSliceBytes,
            &tail,
            &error) ||
        tail.size() != kProbeSliceBytes ||
        !all_zero(tail)) {
        std::cerr << "failed to read zero tail slice: " << error << '\n';
        return 6;
    }

    if (touch_bytes != 0U) {
        std::vector<std::byte> touched;
        if (!reader.read_record_slice(
                0U,
                0U,
                static_cast<std::size_t>(touch_bytes),
                &touched,
                &error) ||
            touched.size() != static_cast<std::size_t>(touch_bytes) ||
            !all_zero(touched)) {
            std::cerr << "failed to touch resident store bytes: " << error << '\n';
            return 7;
        }
        touched.clear();
        touched.shrink_to_fit();
    }

    head.clear();
    head.shrink_to_fit();
    tail.clear();
    tail.shrink_to_fit();

    const auto cache = reader.block_cache_stats();
    if (!cache.ledger_within_hard_limits || !cache.ledger_accounting_clean) {
        std::cerr << "block-cache ledger accounting failed during legacy probe\n";
        return 8;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::cout
        << "{"
        << "\"logical_bytes\":" << stats.corpus.logical_utf8_bytes << ","
        << "\"logical_records\":" << stats.corpus.logical_records << ","
        << "\"segment_count\":" << stats.segment_count << ","
        << "\"chunk_count\":" << stats.chunk_count << ","
        << "\"physical_store_bytes\":" << stats.physical_bytes << ","
        << "\"head_tail_slice_ok\":true,"
        << "\"touch_bytes\":" << touch_bytes << ","
        << "\"cache_resident_bytes\":" << cache.resident_bytes << ","
        << "\"cache_peak_resident_bytes\":" << cache.peak_resident_bytes << ","
        << "\"cache_physical_read_bytes\":" << cache.physical_read_bytes << ","
        << "\"cache_ledger_within_hard_limits\":true,"
        << "\"cache_ledger_accounting_clean\":true"
        << "}\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr
            << "usage: massivedoc_legacy_open_probe prepare <root>\n"
            << "   or: massivedoc_legacy_open_probe open <root> <touch-bytes> <hot-mib> <warm-mib>\n";
        return 2;
    }

    const std::string mode = argv[1];
    const std::filesystem::path root = argv[2];
    if (mode == "prepare") {
        std::string error;
        if (!prepare_fixture(root, &error)) {
            std::cerr << "fixture preparation failed: " << error << '\n';
            return 3;
        }
        std::cout
            << "{"
            << "\"logical_bytes\":" << kLogicalBytes << ","
            << "\"logical_records\":" << kRecordCount << ","
            << "\"segment_bytes\":" << kSegmentBytes << ","
            << "\"segment_count\":" << kSegmentCount
            << "}\n";
        return 0;
    }

    if (mode == "open") {
        if (argc != 6) {
            std::cerr << "open mode requires touch-bytes, hot-mib and warm-mib\n";
            return 2;
        }
        try {
            const auto touch_bytes = static_cast<std::uint64_t>(std::stoull(argv[3]));
            const auto hot_mib = static_cast<std::uint64_t>(std::stoull(argv[4]));
            const auto warm_mib = static_cast<std::uint64_t>(std::stoull(argv[5]));
            return run_open(root, touch_bytes, hot_mib, warm_mib);
        } catch (const std::exception& exception) {
            std::cerr << "invalid numeric probe argument: " << exception.what() << '\n';
            return 2;
        }
    }

    std::cerr << "unknown probe mode\n";
    return 2;
}
