#include "massivedoc_store.hpp"

#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW)
#include "massivedoc_descriptor_shadow.hpp"
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {

using Clock = std::chrono::steady_clock;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "Z2R-3D workload failure: " << message << '\n';
    std::exit(1);
}

template <typename T>
std::optional<T> parse_number(std::string_view text) {
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[maybe_unused]] std::string_view mismatch_name(
#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW)
    zevryon::massivedoc::MassiveDocDescriptorShadowOperation operation
#else
    std::uint32_t operation
#endif
) noexcept {
#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW)
    using Operation = zevryon::massivedoc::MassiveDocDescriptorShadowOperation;
    switch (operation) {
    case Operation::None:
        return "None";
    case Operation::RecordEncode:
        return "RecordEncode";
    case Operation::RecordDecode:
        return "RecordDecode";
    case Operation::ChunkEncode:
        return "ChunkEncode";
    case Operation::ChunkDecode:
        return "ChunkDecode";
    }
#else
    (void)operation;
#endif
    return "Unknown";
}

void reset_shadow() noexcept {
#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW)
    zevryon::massivedoc::reset_massivedoc_descriptor_shadow();
#endif
}

void write_shadow_json() {
#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW)
    const auto snapshot =
        zevryon::massivedoc::massivedoc_descriptor_shadow_snapshot();
    std::cout << "{\"enabled\":true"
              << ",\"record_encode_checks\":" << snapshot.record_encode_checks
              << ",\"record_decode_checks\":" << snapshot.record_decode_checks
              << ",\"chunk_encode_checks\":" << snapshot.chunk_encode_checks
              << ",\"chunk_decode_checks\":" << snapshot.chunk_decode_checks
              << ",\"mismatches\":" << snapshot.mismatches
              << ",\"first_mismatch\":\"" << mismatch_name(snapshot.first_mismatch)
              << "\"}";
#else
    std::cout << "{\"enabled\":false"
              << ",\"record_encode_checks\":0"
              << ",\"record_decode_checks\":0"
              << ",\"chunk_encode_checks\":0"
              << ",\"chunk_decode_checks\":0"
              << ",\"mismatches\":0"
              << ",\"first_mismatch\":\"None\"}";
#endif
}

std::uint64_t record_length(
    std::uint64_t index,
    std::uint64_t logical_bytes,
    std::uint64_t records,
    std::uint64_t giant_record_bytes,
    std::uint64_t giant_index) {
    if (giant_record_bytes != 0U && index == giant_index) {
        return giant_record_bytes;
    }
    const std::uint64_t ordinary_records =
        records - (giant_record_bytes == 0U ? 0U : 1U);
    const std::uint64_t ordinary_bytes = logical_bytes - giant_record_bytes;
    const std::uint64_t base = ordinary_bytes / ordinary_records;
    const std::uint64_t extra = ordinary_bytes % ordinary_records;
    const std::uint64_t ordinary_index =
        giant_record_bytes != 0U && index > giant_index ? index - 1U : index;
    return base + (ordinary_index < extra ? 1U : 0U);
}

std::uint64_t next_state(std::uint64_t state) noexcept {
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
}

void import_store(
    const std::filesystem::path& root,
    std::uint64_t logical_bytes,
    std::uint64_t records,
    std::uint64_t segment_bytes,
    std::uint64_t giant_record_bytes) {
    if (records == 0U || logical_bytes == 0U || segment_bytes == 0U ||
        giant_record_bytes > logical_bytes) {
        fail("invalid import dimensions");
    }
    const std::uint64_t ordinary_records =
        records - (giant_record_bytes == 0U ? 0U : 1U);
    if (ordinary_records == 0U || logical_bytes - giant_record_bytes < ordinary_records) {
        fail("ordinary records must each contain at least one byte");
    }

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    zevryon::massivedoc::StoreConfig config;
    config.segment_bytes = segment_bytes;
    config.records_per_search_block = 8192U;
    zevryon::massivedoc::StoreWriter writer(root, config);
    std::string error;
    const std::uint64_t giant_index = records / 2U;
    std::uint64_t largest = 0U;

    for (std::uint64_t index = 0U; index < records; ++index) {
        const std::uint64_t length = record_length(
            index, logical_bytes, records, giant_record_bytes, giant_index);
        largest = std::max(largest, length);
        std::uint64_t remaining = length;
        std::uint64_t state =
            0x9E3779B97F4A7C15ULL ^ (index * 0xD1B54A32D192ED03ULL);
        const auto reader = [&remaining, &state](std::span<std::byte> destination) {
            const std::size_t amount = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, destination.size()));
            for (std::size_t offset = 0U; offset < amount; ++offset) {
                state = next_state(state);
                destination[offset] = static_cast<std::byte>(state >> 56U);
            }
            remaining -= static_cast<std::uint64_t>(amount);
            return amount;
        };
        const std::uint64_t logical_id = 0x5A00000000000000ULL + index;
        if (!writer.append_stream(logical_id, length, reader, &error)) {
            fail(error);
        }
    }

    zevryon::massivedoc::CorpusMetadata metadata;
    metadata.logical_utf8_bytes = logical_bytes;
    metadata.logical_records = records;
    metadata.logical_nodes = records * 3U;
    metadata.style_runs = records * 2U;
    metadata.resource_references = records / 8U;
    metadata.largest_record_bytes = largest;
    zevryon::massivedoc::StoreStats finalized_stats;
    if (!writer.finalize(metadata, &finalized_stats, &error)) {
        fail(error);
    }

    zevryon::massivedoc::StoreReader persisted_reader(root);
    if (!persisted_reader.open(&error)) {
        fail(error);
    }
    const auto& persisted_stats = persisted_reader.stats();
    if (persisted_stats.payload_sha256 != finalized_stats.payload_sha256 ||
        persisted_stats.corpus.logical_utf8_bytes !=
            finalized_stats.corpus.logical_utf8_bytes ||
        persisted_stats.corpus.logical_records != finalized_stats.corpus.logical_records ||
        persisted_stats.chunk_count != finalized_stats.chunk_count) {
        fail("persisted store stats diverged from finalized store stats");
    }
    std::cout << ",\"store\":"
              << zevryon::massivedoc::stats_json(persisted_stats);
}

void open_store(const std::filesystem::path& root, bool verify) {
    zevryon::massivedoc::StoreReader reader(root);
    std::string error;
    if (!reader.open(&error)) {
        fail(error);
    }
    if (verify && !reader.verify(&error)) {
        fail(error);
    }
    std::cout << ",\"store\":" << zevryon::massivedoc::stats_json(reader.stats())
              << ",\"ok\":true";
}

void export_store(
    const std::filesystem::path& root,
    const std::filesystem::path& output) {
    zevryon::massivedoc::StoreReader reader(root);
    std::string error;
    if (!reader.open(&error) || !reader.export_payload(output, &error)) {
        fail(error);
    }
    std::cout << ",\"store\":" << zevryon::massivedoc::stats_json(reader.stats())
              << ",\"ok\":true";
}

void inject_fault(std::string_view fault) {
#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW)
    namespace md = zevryon::massivedoc;
    if (fault == "record-encode") {
        const std::array<std::byte, 32> bytes{};
        md::verify_massivedoc_record_encoding(1U, 2U, 3U, 4U, 5U, bytes);
    } else if (fault == "record-decode") {
        const std::array<std::byte, 32> bytes{};
        md::verify_massivedoc_record_decoding(bytes, 1U, 2U, 3U, 4U, 5U);
    } else if (fault == "chunk-encode") {
        const std::array<std::byte, 24> bytes{};
        md::verify_massivedoc_chunk_encoding(1U, 2U, 3U, bytes);
    } else if (fault == "chunk-decode") {
        const std::array<std::byte, 24> bytes{};
        md::verify_massivedoc_chunk_decoding(bytes, 1U, 2U, 3U);
    } else {
        fail("unknown fault class");
    }
    std::cout << ",\"fault\":\"" << fault << "\"";
#else
    (void)fault;
    fail("fault injection requires the Rust MassiveDoc codec shadow");
#endif
}

void usage() {
    std::cerr
        << "Usage:\n"
        << "  z2r3d-workload <baseline|shadow> import <store> <logical-bytes>"
           " <records> <segment-bytes> <giant-record-bytes>\n"
        << "  z2r3d-workload <baseline|shadow> open <store>\n"
        << "  z2r3d-workload <baseline|shadow> verify <store>\n"
        << "  z2r3d-workload <baseline|shadow> export <store> <output>\n"
        << "  z2r3d-workload shadow fault"
           " <record-encode|record-decode|chunk-encode|chunk-decode>\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        usage();
        return 2;
    }
    const std::string_view mode = argv[1];
    const std::string_view operation = argv[2];
    if (mode != "baseline" && mode != "shadow") {
        usage();
        return 2;
    }
#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW)
    if (mode != "shadow") {
        fail("shadow build must run in shadow mode");
    }
#else
    if (mode != "baseline") {
        fail("baseline build must run in baseline mode");
    }
#endif

    reset_shadow();
    const auto started = Clock::now();
    std::cout << "{\"schema\":\"zevryon.z2r3d.codec-workload.v1\""
              << ",\"mode\":\"" << mode << "\""
              << ",\"operation\":\"" << operation << "\"";

    if (operation == "import") {
        if (argc != 8) {
            usage();
            return 2;
        }
        const auto logical_bytes = parse_number<std::uint64_t>(argv[4]);
        const auto records = parse_number<std::uint64_t>(argv[5]);
        const auto segment_bytes = parse_number<std::uint64_t>(argv[6]);
        const auto giant_record_bytes = parse_number<std::uint64_t>(argv[7]);
        if (!logical_bytes || !records || !segment_bytes || !giant_record_bytes) {
            fail("invalid numeric import argument");
        }
        import_store(argv[3], *logical_bytes, *records, *segment_bytes, *giant_record_bytes);
    } else if (operation == "open") {
        if (argc != 4) {
            usage();
            return 2;
        }
        open_store(argv[3], false);
    } else if (operation == "verify") {
        if (argc != 4) {
            usage();
            return 2;
        }
        open_store(argv[3], true);
    } else if (operation == "export") {
        if (argc != 5) {
            usage();
            return 2;
        }
        export_store(argv[3], argv[4]);
    } else if (operation == "fault") {
        if (argc != 4 || mode != "shadow") {
            usage();
            return 2;
        }
        inject_fault(argv[3]);
    } else {
        usage();
        return 2;
    }

    const double seconds =
        std::chrono::duration<double>(Clock::now() - started).count();
    std::cout << ",\"seconds\":" << seconds << ",\"shadow\":";
    write_shadow_json();
    std::cout << "}\n";
    return 0;
}
