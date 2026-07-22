#include "massivedoc_store.hpp"

#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
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

void usage() {
    std::cerr
        << "Usage:\n"
        << "  zevryon-massivedoc import <corpus.zmdoc> <store-dir> [segment-mib]\n"
        << "  zevryon-massivedoc stats <store-dir>\n"
        << "  zevryon-massivedoc verify <store-dir>\n"
        << "  zevryon-massivedoc search <store-dir> <query> [max-hits]\n"
        << "  zevryon-massivedoc export <store-dir> <output.bin>\n";
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
            if (!mib || *mib == 0U) {
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
        std::cout << "{\"operation\":\"search\",\"seconds\":" << elapsed << ",\"count\":" << hits.size() << ",\"hits\":[";
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
