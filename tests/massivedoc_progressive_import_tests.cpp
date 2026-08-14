#include "massivedoc_progressive_import.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr std::array<char, 8> kCorpusMagic{'Z', 'M', 'D', 'O', 'C', '0', '0', '1'};

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

bool write_bytes(
    std::ofstream& stream,
    std::span<const std::byte> bytes) {
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream);
}

std::vector<std::byte> repeated_payload(char value, std::size_t size) {
    return std::vector<std::byte>(
        size,
        static_cast<std::byte>(static_cast<unsigned char>(value)));
}

bool write_corpus(
    const std::filesystem::path& path,
    const std::vector<std::vector<std::byte>>& payloads,
    std::string* error) {
    std::uint64_t logical_bytes = 0U;
    std::uint64_t largest = 0U;
    for (const auto& payload : payloads) {
        logical_bytes += static_cast<std::uint64_t>(payload.size());
        largest = std::max<std::uint64_t>(
            largest,
            static_cast<std::uint64_t>(payload.size()));
    }

    std::vector<std::byte> header;
    header.reserve(64U);
    for (const char value : kCorpusMagic) {
        header.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    append_u64(header, logical_bytes);
    append_u64(header, static_cast<std::uint64_t>(payloads.size()));
    append_u64(header, static_cast<std::uint64_t>(payloads.size()) * 8U);
    append_u64(header, static_cast<std::uint64_t>(payloads.size()) * 4U);
    append_u64(header, 1U);
    append_u64(header, largest);
    append_u64(header, 0U);
    if (header.size() != 64U) {
        *error = "test corpus header size mismatch";
        return false;
    }

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream || !write_bytes(stream, header)) {
        *error = "cannot write test corpus header";
        return false;
    }
    for (std::size_t index = 0U; index < payloads.size(); ++index) {
        const auto& payload = payloads[index];
        if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
            *error = "test payload exceeds ZMDOC record width";
            return false;
        }
        std::vector<std::byte> record_header;
        record_header.reserve(12U);
        append_u64(record_header, 100U + static_cast<std::uint64_t>(index));
        append_u32(record_header, static_cast<std::uint32_t>(payload.size()));
        if (record_header.size() != 12U ||
            !write_bytes(stream, record_header) ||
            !write_bytes(stream, payload)) {
            *error = "cannot write test corpus record";
            return false;
        }
    }
    stream.flush();
    if (!stream) {
        *error = "cannot flush test corpus";
        return false;
    }
    stream.close();
    if (stream.fail()) {
        *error = "cannot close test corpus";
        return false;
    }
    return true;
}

bool read_record_bytes(
    const zevryon::massivedoc::StoreReader& reader,
    std::uint64_t index,
    std::vector<std::byte>* output,
    std::string* error) {
    output->clear();
    return reader.read_record(
        index,
        [output](std::span<const std::byte> bytes) {
            output->insert(output->end(), bytes.begin(), bytes.end());
            return true;
        },
        error);
}

bool fail_callback(std::string* error, const std::string& message) {
    *error = message;
    return false;
}

} // namespace

int main() {
    using namespace zevryon::massivedoc;

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("zevryon-progressive-import-" + std::to_string(unique));
    const std::filesystem::path corpus_path = root / "fixture.zmdoc";
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path preview_root = root / "preview";

    std::error_code filesystem_error;
    std::filesystem::remove_all(root, filesystem_error);
    filesystem_error.clear();
    std::filesystem::create_directories(root, filesystem_error);
    if (filesystem_error) {
        std::cerr << "cannot create progressive-import test root\n";
        return 1;
    }

    const std::vector<std::vector<std::byte>> payloads{
        repeated_payload('A', 150U),
        repeated_payload('B', 70U),
        repeated_payload('C', 90U)};
    std::string error;
    if (!write_corpus(corpus_path, payloads, &error)) {
        std::cerr << error << '\n';
        return 1;
    }

    ProgressiveImportConfig config;
    config.store.segment_bytes = 64U;
    config.store.records_per_search_block = 4U;
    config.preview_records = 1U;
    config.preview_arena.records_per_block = 2U;

    bool callback_seen = false;
    bool primary_uncommitted_seen = false;
    bool preview_verified = false;
    bool viewport_materialized = false;
    StoreStats final_stats{};

    const bool imported = import_zmdoc_corpus_progressive(
        corpus_path,
        store_root,
        preview_root,
        config,
        [&](const ProgressivePreviewInfo& info, std::string* callback_error) {
            callback_seen = true;
            if (info.total_records != 3U || info.remaining_records != 2U) {
                return fail_callback(
                    callback_error,
                    "preview callback did not occur before remaining import work");
            }
            if (info.store.corpus.logical_records != 1U ||
                info.store.corpus.logical_utf8_bytes !=
                    static_cast<std::uint64_t>(payloads[0].size()) ||
                info.store.segment_count != 3U ||
                info.store.chunk_count != 3U ||
                info.store.search_block_count != 1U ||
                info.arena.logical_records != 1U) {
                return fail_callback(
                    callback_error,
                    "preview snapshot statistics differ from the first-record prefix");
            }

            StoreReader primary(store_root);
            std::string primary_error;
            if (primary.open(&primary_error)) {
                return fail_callback(
                    callback_error,
                    "primary store became readable before full import completion");
            }
            primary_uncommitted_seen = true;

            StoreReader preview(info.root);
            if (!preview.open(callback_error) || !preview.verify(callback_error)) {
                return false;
            }
            std::vector<std::byte> record;
            if (!read_record_bytes(preview, 0U, &record, callback_error) ||
                record != payloads[0]) {
                return fail_callback(
                    callback_error,
                    "preview record payload differs from the imported prefix");
            }
            std::string search_error;
            const auto hits = preview.find("AAAA", 8U, &search_error);
            if (!search_error.empty() || hits.empty() || hits.front().record_index != 0U) {
                return fail_callback(
                    callback_error,
                    "preview partial search block is not queryable");
            }
            preview_verified = true;

            CompactArenaReader arena(info.root);
            if (!arena.open(callback_error)) {
                return false;
            }
            ViewportResult viewport;
            if (!arena.materialize(
                    0U,
                    720U * 256U,
                    0U,
                    16U,
                    &viewport,
                    callback_error)) {
                return false;
            }
            if (viewport.records.empty() ||
                viewport.records.front().logical_id != 100U ||
                viewport.records.front().source_record_index != 0U) {
                return fail_callback(
                    callback_error,
                    "first preview viewport did not materialize the prefix record");
            }
            viewport_materialized = true;
            return true;
        },
        &final_stats,
        &error);

    if (!imported) {
        std::cerr << "progressive import failed: " << error << '\n';
        return 1;
    }
    if (!callback_seen || !primary_uncommitted_seen ||
        !preview_verified || !viewport_materialized) {
        std::cerr << "progressive preview ordering proof was incomplete\n";
        return 1;
    }
    if (final_stats.corpus.logical_records != 3U) {
        std::cerr << "final store record count mismatch\n";
        return 1;
    }

    StoreReader final_reader(store_root);
    if (!final_reader.open(&error) || !final_reader.verify(&error)) {
        std::cerr << "final store verification failed: " << error << '\n';
        return 1;
    }
    StoreReader preview_reader(preview_root);
    if (!preview_reader.open(&error) || !preview_reader.verify(&error)) {
        std::cerr << "preview lost independent readability after final import: " << error << '\n';
        return 1;
    }
    if (preview_reader.stats().corpus.logical_records != 1U) {
        std::cerr << "preview authority mutated after final import\n";
        return 1;
    }

    std::filesystem::remove_all(root, filesystem_error);
    std::cout
        << "Zevryon MassiveDoc progressive preview tests passed: "
        << "viewport usable with 2 records still pending\n";
    return 0;
}
