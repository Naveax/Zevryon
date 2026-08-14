#include "massivedoc_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <thread>

namespace {

constexpr std::uint64_t kMib = 1024ULL * 1024ULL;
constexpr std::uint64_t kFixtureBytes = 8ULL * kMib;

int prepare_store(const std::filesystem::path& root) {
    std::error_code filesystem_error;
    std::filesystem::remove_all(root, filesystem_error);

    zevryon::massivedoc::StoreWriter writer(root);
    std::uint64_t produced = 0U;
    std::string error;
    if (!writer.append_stream(
            1U,
            kFixtureBytes,
            [&produced](std::span<std::byte> destination) -> std::size_t {
                for (std::size_t index = 0U; index < destination.size(); ++index) {
                    const std::uint64_t position = produced + static_cast<std::uint64_t>(index);
                    destination[index] = static_cast<std::byte>(
                        static_cast<unsigned char>((position * 29U + 17U) & 0xffU));
                }
                produced += static_cast<std::uint64_t>(destination.size());
                return destination.size();
            },
            &error)) {
        std::cerr << "cold PSS fixture append failed: " << error << '\n';
        return 3;
    }

    zevryon::massivedoc::CorpusMetadata metadata;
    metadata.logical_utf8_bytes = kFixtureBytes;
    metadata.logical_records = 1U;
    metadata.logical_nodes = 1U;
    metadata.style_runs = 0U;
    metadata.resource_references = 0U;
    metadata.largest_record_bytes = kFixtureBytes;
    zevryon::massivedoc::StoreStats stats;
    if (!writer.finalize(metadata, &stats, &error)) {
        std::cerr << "cold PSS fixture finalize failed: " << error << '\n';
        return 4;
    }
    if (stats.corpus.logical_utf8_bytes != kFixtureBytes || stats.segment_count != 1U) {
        std::cerr << "cold PSS fixture stats mismatch\n";
        return 5;
    }
    std::cout
        << "{\"logical_bytes\":" << stats.corpus.logical_utf8_bytes
        << ",\"segment_count\":" << stats.segment_count
        << ",\"physical_bytes\":" << stats.physical_bytes << "}\n";
    return 0;
}

int hold_store(
    const std::filesystem::path& root,
    std::uint64_t touch_bytes,
    std::uint64_t cold_mib) {
    if (cold_mib > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(kMib) ||
        touch_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        std::cerr << "cold PSS probe argument exceeds size_t\n";
        return 2;
    }

    zevryon::massivedoc::StoreReadConfig config;
    config.cold_window_bytes =
        static_cast<std::size_t>(cold_mib) * static_cast<std::size_t>(kMib);
    zevryon::massivedoc::StoreReader reader(root, config);
    std::string error;
    if (!reader.open(&error)) {
        std::cerr << "cold PSS StoreReader::open failed: " << error << '\n';
        return 3;
    }
    if (reader.stats().corpus.logical_utf8_bytes != kFixtureBytes ||
        reader.stats().segment_count != 1U) {
        std::cerr << "cold PSS opened store stats mismatch\n";
        return 4;
    }

    reader.evict_block_cache_to_cold();
    if (touch_bytes != 0U) {
        if (!reader.touch_record_slice_cold(
                0U,
                0U,
                static_cast<std::size_t>(touch_bytes),
                &error)) {
            std::cerr << "cold PSS mapped touch failed: " << error << '\n';
            return 5;
        }
    }

    const auto block = reader.block_cache_stats();
    const auto cold = reader.cold_window_stats();
    if (block.resident_bytes != 0U) {
        std::cerr << "hot/warm block cache remained resident in cold PSS probe\n";
        return 6;
    }
    if (touch_bytes != 0U) {
        if (cold.mapped_bytes < touch_bytes ||
            cold.touched_bytes < touch_bytes ||
            !cold.ledger_within_hard_limits ||
            !cold.ledger_accounting_clean) {
            std::cerr << "cold mapped window accounting mismatch\n";
            return 7;
        }
    } else if (cold.mapped_bytes != 0U) {
        std::cerr << "baseline cold PSS probe unexpectedly mapped store pages\n";
        return 8;
    }

    std::cout << "COLD_READY\n" << std::flush;
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    std::cout
        << "{"
        << "\"logical_bytes\":" << reader.stats().corpus.logical_utf8_bytes << ","
        << "\"touch_bytes\":" << touch_bytes << ","
        << "\"hot_warm_resident_bytes\":" << block.resident_bytes << ","
        << "\"cold_mapped_bytes\":" << cold.mapped_bytes << ","
        << "\"cold_peak_mapped_bytes\":" << cold.peak_mapped_bytes << ","
        << "\"cold_touched_bytes\":" << cold.touched_bytes << ","
        << "\"cold_mapping_hits\":" << cold.mapping_hits << ","
        << "\"cold_mapping_misses\":" << cold.mapping_misses << ","
        << "\"cold_remaps\":" << cold.remaps << ","
        << "\"cold_ledger_within_hard_limits\":"
        << (cold.ledger_within_hard_limits ? "true" : "false") << ","
        << "\"cold_ledger_accounting_clean\":"
        << (cold.ledger_accounting_clean ? "true" : "false")
        << "}\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr
            << "usage: massivedoc_cold_pss_probe prepare <root>\n"
            << "   or: massivedoc_cold_pss_probe hold <root> <touch-bytes> <cold-mib>\n";
        return 2;
    }
    const std::string mode = argv[1];
    const std::filesystem::path root = argv[2];
    if (mode == "prepare") {
        return prepare_store(root);
    }
    if (mode == "hold") {
        if (argc != 5) {
            std::cerr << "hold mode requires touch-bytes and cold-mib\n";
            return 2;
        }
        try {
            const auto touch_bytes = static_cast<std::uint64_t>(std::stoull(argv[3]));
            const auto cold_mib = static_cast<std::uint64_t>(std::stoull(argv[4]));
            return hold_store(root, touch_bytes, cold_mib);
        } catch (const std::exception& exception) {
            std::cerr << "invalid cold PSS probe argument: " << exception.what() << '\n';
            return 2;
        }
    }
    std::cerr << "unknown cold PSS probe mode\n";
    return 2;
}
