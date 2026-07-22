#include "compact_document.hpp"
#include "massivedoc_store.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace {

template <typename T>
std::optional<T> parse_number(std::string_view text) {
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint64_t> pixels_to_q8(std::string_view text) {
    const auto pixels = parse_number<std::uint64_t>(text);
    if (!pixels || *pixels > std::numeric_limits<std::uint64_t>::max() / 256U) {
        return std::nullopt;
    }
    return *pixels * 256U;
}

void usage() {
    std::cerr
        << "Usage:\n"
        << "  zevryon-massivedoc import <corpus.zmdoc> <store-dir> [segment-mib]\n"
        << "  zevryon-massivedoc stats <store-dir>\n"
        << "  zevryon-massivedoc verify <store-dir>\n"
        << "  zevryon-massivedoc search <store-dir> <query> [max-hits]\n"
        << "  zevryon-massivedoc export <store-dir> <output.bin>\n"
        << "  zevryon-massivedoc arena-build <store-dir> [bytes-per-line] [line-height-px]\n"
        << "  zevryon-massivedoc arena-stats <store-dir>\n"
        << "  zevryon-massivedoc viewport <store-dir> <scroll-y-px> <height-px> [overscan-px] [max-records]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        usage();
        return 2;
    }
    const std::string command = argv[1];
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    if (command == "import") {
        if (argc < 4 || argc > 5) {
            usage();
            return 2;
        }
        zevryon::massivedoc::StoreConfig config;
        if (argc == 5) {
            const auto mib = parse_number<std::uint64_t>(argv[4]);
            if (!mib || *mib == 0U || *mib > std::numeric_limits<std::uint64_t>::max() / (1024ULL * 1024ULL)) {
                std::cerr << "invalid segment size\n";
                return 2;
            }
            config.segment_bytes = *mib * 1024ULL * 1024ULL;
        }
        zevryon::massivedoc::StoreStats stats;
        if (!zevryon::massivedoc::import_zmdoc_corpus(argv[2], argv[3], config, &stats, &error)) {
            std::cerr << "import failed: " << error << '\n';
            return 1;
        }
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "{\"operation\":\"import\",\"seconds\":" << elapsed
                  << ",\"store\":" << zevryon::massivedoc::stats_json(stats) << "}\n";
        return 0;
    }

    if (command == "arena-build") {
        if (argc < 3 || argc > 5) {
            usage();
            return 2;
        }
        zevryon::massivedoc::ArenaConfig config;
        if (argc >= 4) {
            const auto bytes_per_line = parse_number<std::uint32_t>(argv[3]);
            if (!bytes_per_line || *bytes_per_line == 0U) {
                std::cerr << "invalid bytes-per-line\n";
                return 2;
            }
            config.estimated_bytes_per_line = *bytes_per_line;
        }
        if (argc == 5) {
            const auto line_height = parse_number<std::uint32_t>(argv[4]);
            if (!line_height || *line_height == 0U || *line_height > std::numeric_limits<std::uint32_t>::max() / 256U) {
                std::cerr << "invalid line-height\n";
                return 2;
            }
            config.line_height_q8 = *line_height * 256U;
        }
        zevryon::massivedoc::ArenaStats stats;
        if (!zevryon::massivedoc::build_compact_arena(argv[2], config, &stats, &error)) {
            std::cerr << "arena build failed: " << error << '\n';
            return 1;
        }
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "{\"operation\":\"arena-build\",\"seconds\":" << elapsed
                  << ",\"arena\":" << zevryon::massivedoc::arena_stats_json(stats) << "}\n";
        return 0;
    }

    if (command == "arena-stats") {
        zevryon::massivedoc::CompactArenaReader arena(argv[2]);
        if (!arena.open(&error)) {
            std::cerr << "arena open failed: " << error << '\n';
            return 1;
        }
        std::cout << zevryon::massivedoc::arena_stats_json(arena.stats()) << '\n';
        return 0;
    }

    if (command == "viewport") {
        if (argc < 5 || argc > 7) {
            usage();
            return 2;
        }
        const auto scroll_y = pixels_to_q8(argv[3]);
        const auto height = pixels_to_q8(argv[4]);
        const auto overscan = argc >= 6 ? pixels_to_q8(argv[5]) : std::optional<std::uint64_t>{720U * 256U};
        const auto max_records = argc == 7 ? parse_number<std::size_t>(argv[6]) : std::optional<std::size_t>{512U};
        if (!scroll_y || !height || *height == 0U || !overscan || !max_records || *max_records == 0U) {
            std::cerr << "invalid viewport arguments\n";
            return 2;
        }
        zevryon::massivedoc::CompactArenaReader arena(argv[2]);
        if (!arena.open(&error)) {
            std::cerr << "arena open failed: " << error << '\n';
            return 1;
        }
        zevryon::massivedoc::ViewportResult result;
        if (!arena.materialize(*scroll_y, *height, *overscan, *max_records, &result, &error)) {
            std::cerr << "viewport failed: " << error << '\n';
            return 1;
        }
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "{\"operation\":\"viewport\",\"seconds\":" << elapsed
                  << ",\"viewport\":" << zevryon::massivedoc::viewport_json(result) << "}\n";
        return 0;
    }

    zevryon::massivedoc::StoreReader reader(argv[2]);
    if (!reader.open(&error)) {
        std::cerr << "open failed: " << error << '\n';
        return 1;
    }
    if (command == "stats") {
        std::cout << zevryon::massivedoc::stats_json(reader.stats()) << '\n';
        return 0;
    }
    if (command == "verify") {
        if (!reader.verify(&error)) {
            std::cerr << "verify failed: " << error << '\n';
            return 1;
        }
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "{\"operation\":\"verify\",\"seconds\":" << elapsed << ",\"ok\":true}\n";
        return 0;
    }
    if (command == "export") {
        if (argc != 4) {
            usage();
            return 2;
        }
        if (!reader.export_payload(argv[3], &error)) {
            std::cerr << "export failed: " << error << '\n';
            return 1;
        }
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "{\"operation\":\"export\",\"seconds\":" << elapsed << ",\"ok\":true}\n";
        return 0;
    }
    if (command == "search") {
        if (argc < 4 || argc > 5) {
            usage();
            return 2;
        }
        std::size_t max_hits = 100U;
        if (argc == 5) {
            const auto parsed = parse_number<std::size_t>(argv[4]);
            if (!parsed || *parsed == 0U) {
                std::cerr << "invalid max-hits\n";
                return 2;
            }
            max_hits = *parsed;
        }
        const auto hits = reader.find(argv[3], max_hits, &error);
        if (!error.empty()) {
            std::cerr << "search failed: " << error << '\n';
            return 1;
        }
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "{\"operation\":\"search\",\"seconds\":" << elapsed << ",\"count\":" << hits.size()
                  << ",\"hits\":[";
        bool first = true;
        for (const auto& hit : hits) {
            if (!first) {
                std::cout << ',';
            }
            first = false;
            std::cout << "{\"record_index\":" << hit.record_index << ",\"logical_id\":" << hit.logical_id
                      << ",\"byte_offset\":" << hit.byte_offset << '}';
        }
        std::cout << "]}\n";
        return 0;
    }
    usage();
    return 2;
}
