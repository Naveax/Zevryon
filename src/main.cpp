#include "compact_document.hpp"
#include "layout_checkpoint.hpp"
#include "layout_window.hpp"
#include "massivedoc_store.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
        << "  zevryon-massivedoc checkpoint-build <store-dir> <record-index> <width-px> [stride-kib]\n"
        << "  zevryon-massivedoc checkpoint-window <store-dir> <record-index> <local-y-px>"
           " <width-px> <height-px> [max-fragments] [stride-kib]\n"
        << "  zevryon-massivedoc height-update <store-dir> <record-index> <height-px>\n"
        << "  zevryon-massivedoc viewport <store-dir> <scroll-y-px> <height-px> [overscan-px] [max-records]\n"
        << "  zevryon-massivedoc layout-window <store-dir> <scroll-y-px> <width-px> <height-px>"
           " [overscan-px] [max-fragments] [cache-mb]\n";
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
            if (!mib || *mib == 0U ||
                *mib > std::numeric_limits<std::uint64_t>::max() / (1024ULL * 1024ULL)) {
                std::cerr << "invalid segment size\n";
                return 2;
            }
            config.segment_bytes = *mib * 1024ULL * 1024ULL;
        }
        zevryon::massivedoc::StoreStats stats;
        if (!zevryon::massivedoc::import_zmdoc_corpus(
                argv[2], argv[3], config, &stats, &error)) {
            std::cerr << "import failed: " << error << '\n';
            return 1;
        }
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
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
            if (!line_height || *line_height == 0U ||
                *line_height > std::numeric_limits<std::uint32_t>::max() / 256U) {
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
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
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

    if (command == "checkpoint-build") {
        if (argc < 5 || argc > 6) {
            usage();
            return 2;
        }
        const auto record_index = parse_number<std::uint64_t>(argv[3]);
        const auto width_q8 = pixels_to_q8(argv[4]);
        const auto stride_kib =
            argc == 6 ? parse_number<std::uint32_t>(argv[5])
                      : std::optional<std::uint32_t>{256U};
        if (!record_index || !width_q8 || *width_q8 == 0U ||
            *width_q8 > std::numeric_limits<std::uint32_t>::max() || !stride_kib ||
            *stride_kib == 0U ||
            *stride_kib > std::numeric_limits<std::uint32_t>::max() / 1024U) {
            std::cerr << "invalid checkpoint build arguments\n";
            return 2;
        }
        zevryon::massivedoc::LayoutCheckpointConfig config;
        config.width_q8 = static_cast<std::uint32_t>(*width_q8);
        config.target_stride_bytes = *stride_kib * 1024U;
        zevryon::massivedoc::LayoutCheckpointStats stats;
        if (!zevryon::massivedoc::build_layout_checkpoint(
                argv[2], *record_index, config, &stats, &error)) {
            std::cerr << "checkpoint build failed: " << error << '\n';
            return 1;
        }
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "{\"operation\":\"checkpoint-build\",\"seconds\":" << elapsed
                  << ",\"checkpoint\":"
                  << zevryon::massivedoc::layout_checkpoint_stats_json(stats) << "}\n";
        return 0;
    }

    if (command == "checkpoint-window") {
        if (argc < 7 || argc > 9) {
            usage();
            return 2;
        }
        const auto record_index = parse_number<std::uint64_t>(argv[3]);
        const auto local_y_q8 = pixels_to_q8(argv[4]);
        const auto width_q8 = pixels_to_q8(argv[5]);
        const auto height_q8 = pixels_to_q8(argv[6]);
        const auto max_fragments =
            argc >= 8 ? parse_number<std::size_t>(argv[7])
                      : std::optional<std::size_t>{512U};
        const auto stride_kib =
            argc == 9 ? parse_number<std::uint32_t>(argv[8])
                      : std::optional<std::uint32_t>{256U};
        if (!record_index || !local_y_q8 || !width_q8 || *width_q8 == 0U ||
            *width_q8 > std::numeric_limits<std::uint32_t>::max() || !height_q8 ||
            *height_q8 == 0U || !max_fragments || *max_fragments == 0U || !stride_kib ||
            *stride_kib == 0U ||
            *stride_kib > std::numeric_limits<std::uint32_t>::max() / 1024U ||
            *local_y_q8 > std::numeric_limits<std::uint64_t>::max() - *height_q8) {
            std::cerr << "invalid checkpoint window arguments\n";
            return 2;
        }
        zevryon::massivedoc::LayoutCheckpointConfig config;
        config.width_q8 = static_cast<std::uint32_t>(*width_q8);
        config.target_stride_bytes = *stride_kib * 1024U;
        zevryon::massivedoc::LayoutCheckpointIndex index;
        if (!index.open(argv[2], *record_index, config, &error)) {
            std::cerr << "checkpoint open failed: " << error << '\n';
            return 1;
        }
        const auto& checkpoint = index.stats();
        zevryon::massivedoc::MaterializedRecord record;
        record.record_index = checkpoint.record_index;
        record.logical_id = checkpoint.logical_id;
        record.height_q8 = checkpoint.measured_height_q8;
        record.source_bytes = checkpoint.source_bytes;
        std::vector<zevryon::massivedoc::LayoutFragment> fragments;
        std::uint64_t source_bytes_read = 0U;
        std::uint64_t checkpoint_source_offset = 0U;
        bool truncated = false;
        if (!zevryon::massivedoc::scan_layout_window_from_checkpoint(
                argv[2],
                record,
                index,
                *local_y_q8,
                *local_y_q8 + *height_q8,
                *max_fragments,
                &fragments,
                &source_bytes_read,
                &checkpoint_source_offset,
                &truncated,
                &error)) {
            std::cerr << "checkpoint window failed: " << error << '\n';
            return 1;
        }
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "{\"operation\":\"checkpoint-window\",\"seconds\":" << elapsed
                  << ",\"record_index\":" << checkpoint.record_index
                  << ",\"source_bytes\":" << checkpoint.source_bytes
                  << ",\"source_bytes_read\":" << source_bytes_read
                  << ",\"checkpoint_source_offset\":" << checkpoint_source_offset
                  << ",\"checkpoint_entries\":" << checkpoint.entry_count
                  << ",\"checkpoint_physical_bytes\":" << checkpoint.physical_bytes
                  << ",\"truncated\":" << (truncated ? "true" : "false")
                  << ",\"fragment_count\":" << fragments.size() << ",\"fragments\":[";
        bool first_fragment = true;
        for (const auto& fragment : fragments) {
            if (!first_fragment) {
                std::cout << ',';
            }
            first_fragment = false;
            std::cout << "{\"source_start\":" << fragment.source_start
                      << ",\"source_end\":" << fragment.source_end
                      << ",\"y_q8\":" << fragment.y_q8
                      << ",\"height_q8\":" << fragment.height_q8 << '}';
        }
        std::cout << "]}\n";
        return 0;
    }

    if (command == "height-update") {
        if (argc != 5) {
            usage();
            return 2;
        }
        const auto record_index = parse_number<std::uint64_t>(argv[3]);
        const auto height_q8 = pixels_to_q8(argv[4]);
        if (!record_index || !height_q8 || *height_q8 == 0U ||
            *height_q8 > std::numeric_limits<std::uint32_t>::max()) {
            std::cerr << "invalid height update arguments\n";
            return 2;
        }
        zevryon::massivedoc::CompactArenaReader arena(argv[2]);
        if (!arena.open(&error)) {
            std::cerr << "arena open failed: " << error << '\n';
            return 1;
        }
        zevryon::massivedoc::HeightUpdateResult result;
        if (!arena.update_height(
                *record_index,
                static_cast<std::uint32_t>(*height_q8),
                &result,
                &error)) {
            std::cerr << "height update failed: " << error << '\n';
            return 1;
        }
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "{\"operation\":\"height-update\",\"seconds\":" << elapsed
                  << ",\"update\":" << zevryon::massivedoc::height_update_json(result) << "}\n";
        return 0;
    }

    if (command == "viewport") {
        if (argc < 5 || argc > 7) {
            usage();
            return 2;
        }
        const auto scroll_y = pixels_to_q8(argv[3]);
        const auto height = pixels_to_q8(argv[4]);
        const auto overscan =
            argc >= 6 ? pixels_to_q8(argv[5])
                      : std::optional<std::uint64_t>{720U * 256U};
        const auto max_records =
            argc == 7 ? parse_number<std::size_t>(argv[6])
                      : std::optional<std::size_t>{512U};
        if (!scroll_y || !height || *height == 0U || !overscan || !max_records ||
            *max_records == 0U) {
            std::cerr << "invalid viewport arguments\n";
            return 2;
        }
        zevryon::massivedoc::CompactArenaReader arena(argv[2]);
        if (!arena.open(&error)) {
            std::cerr << "arena open failed: " << error << '\n';
            return 1;
        }
        zevryon::massivedoc::ViewportResult result;
        if (!arena.materialize(
                *scroll_y, *height, *overscan, *max_records, &result, &error)) {
            std::cerr << "viewport failed: " << error << '\n';
            return 1;
        }
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "{\"operation\":\"viewport\",\"seconds\":" << elapsed
                  << ",\"viewport\":" << zevryon::massivedoc::viewport_json(result) << "}\n";
        return 0;
    }

    if (command == "layout-window") {
        if (argc < 6 || argc > 9) {
            usage();
            return 2;
        }
        const auto scroll_y = pixels_to_q8(argv[3]);
        const auto width = pixels_to_q8(argv[4]);
        const auto height = pixels_to_q8(argv[5]);
        const auto overscan =
            argc >= 7 ? pixels_to_q8(argv[6])
                      : std::optional<std::uint64_t>{720U * 256U};
        const auto max_fragments =
            argc >= 8 ? parse_number<std::size_t>(argv[7])
                      : std::optional<std::size_t>{512U};
        const auto cache_mb =
            argc == 9 ? parse_number<std::size_t>(argv[8])
                      : std::optional<std::size_t>{8U};
        if (!scroll_y || !width || *width == 0U ||
            *width > std::numeric_limits<std::uint32_t>::max() || !height ||
            *height == 0U || !overscan || !max_fragments || *max_fragments == 0U ||
            !cache_mb || *cache_mb == 0U ||
            *cache_mb > std::numeric_limits<std::size_t>::max() / 1000000U) {
            std::cerr << "invalid layout window arguments\n";
            return 2;
        }
        zevryon::massivedoc::LayoutConfig config;
        config.max_cache_bytes = *cache_mb * 1000000U;
        zevryon::massivedoc::LayoutWindowEngine engine(argv[2], config);
        if (!engine.open(&error)) {
            std::cerr << "layout open failed: " << error << '\n';
            return 1;
        }
        zevryon::massivedoc::LayoutWindowResult result;
        if (!engine.layout(
                *scroll_y,
                static_cast<std::uint32_t>(*width),
                *height,
                *overscan,
                *max_fragments,
                &result,
                &error)) {
            std::cerr << "layout window failed: " << error << '\n';
            return 1;
        }
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "{\"operation\":\"layout-window\",\"seconds\":" << elapsed
                  << ",\"layout\":" << zevryon::massivedoc::layout_window_json(result)
                  << "}\n";
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
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "{\"operation\":\"verify\",\"seconds\":" << elapsed
                  << ",\"ok\":true}\n";
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
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "{\"operation\":\"export\",\"seconds\":" << elapsed
                  << ",\"ok\":true}\n";
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
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "{\"operation\":\"search\",\"seconds\":" << elapsed
                  << ",\"count\":" << hits.size() << ",\"hits\":[";
        bool first = true;
        for (const auto& hit : hits) {
            if (!first) {
                std::cout << ',';
            }
            first = false;
            std::cout << "{\"record_index\":" << hit.record_index
                      << ",\"logical_id\":" << hit.logical_id
                      << ",\"byte_offset\":" << hit.byte_offset << '}';
        }
        std::cout << "]}\n";
        return 0;
    }
    usage();
    return 2;
}
