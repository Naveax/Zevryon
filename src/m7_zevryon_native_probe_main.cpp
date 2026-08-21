#include "device_frame_profile.hpp"
#include "massivedoc_store.hpp"
#include "shared_record_length_authority.hpp"
#include "shared_source_prefetch_pool.hpp"
#include "zenith_tab_runtime.hpp"
#include "zenith_tab_runtime_profile.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace {
using namespace zevryon::massivedoc;

template <typename T>
std::optional<T> parse_number(std::string_view text) {
    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
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

std::optional<DeviceFrameProfile> parse_profile(std::string_view text) {
    if (text == "legacy-phone") return DeviceFrameProfile::LegacyPhone;
    if (text == "mid-phone") return DeviceFrameProfile::MidPhone;
    if (text == "modern-phone") return DeviceFrameProfile::ModernPhone;
    if (text == "desktop") return DeviceFrameProfile::Desktop;
    return std::nullopt;
}

double elapsed_ms(
    std::chrono::steady_clock::time_point started,
    std::chrono::steady_clock::time_point finished) noexcept {
    return std::chrono::duration<double, std::milli>(finished - started).count();
}

int run_preindexed(int argc, char** argv) {
    if (argc != 9) {
        std::cerr
            << "Usage: zevryon-m7-native-probe preindexed <store-dir> <profile>"
               " <width-px> <height-px> <overscan-px> <max-fragments>"
               " open-plus-first-layout-v1\n";
        return 2;
    }
    const std::filesystem::path store_root(argv[2]);
    const auto profile = parse_profile(argv[3]);
    const auto width_q8 = pixels_to_q8(argv[4]);
    const auto height_q8 = pixels_to_q8(argv[5]);
    const auto overscan_q8 = pixels_to_q8(argv[6]);
    const auto max_fragments = parse_number<std::size_t>(argv[7]);
    const std::string_view boundary(argv[8]);
    if (!profile || !width_q8 || *width_q8 == 0U ||
        *width_q8 > std::numeric_limits<std::uint32_t>::max() ||
        !height_q8 || *height_q8 == 0U || !overscan_q8 ||
        !max_fragments || *max_fragments == 0U ||
        boundary != "open-plus-first-layout-v1") {
        std::cerr << "invalid M7 preindexed probe arguments\n";
        return 2;
    }

    SharedRecordLengthAuthority record_lengths;
    SharedSourcePrefetchPoolConfig pool_config;
    pool_config.record_length_authority = &record_lengths;
    SharedSourcePrefetchPool pool(pool_config);
    ZenithTabRuntimeConfig runtime_config = make_zenith_tab_runtime_config(*profile);
    runtime_config.record_length_authority = &record_lengths;
    if (!pool.valid() || !runtime_config.valid()) {
        std::cerr << "invalid M7 preindexed runtime configuration\n";
        return 1;
    }

    ZenithTabRuntime runtime(store_root, &pool, 1U, runtime_config);
    std::string error;
    LayoutWindowResult result;
    bool used_checkpoint = false;
    const auto started = std::chrono::steady_clock::now();
    if (!runtime.open(&error)) {
        std::cerr << "M7 preindexed runtime open failed: " << error << '\n';
        return 1;
    }
    if (!runtime.set_activity(
            FrameVisibility::Visible,
            FramePressure::Normal,
            0,
            &error)) {
        std::cerr << "M7 preindexed activity setup failed: " << error << '\n';
        return 1;
    }
    if (!runtime.layout(
            0U,
            static_cast<std::uint32_t>(*width_q8),
            *height_q8,
            *overscan_q8,
            *max_fragments,
            &result,
            &used_checkpoint,
            &error)) {
        std::cerr << "M7 preindexed first layout failed: " << error << '\n';
        return 1;
    }
    const auto finished = std::chrono::steady_clock::now();
    if (!used_checkpoint || result.fragments.empty()) {
        std::cerr << "M7 preindexed viewport was not checkpoint-backed and usable\n";
        return 1;
    }

    std::cout << std::fixed << std::setprecision(9)
              << "{\"operation\":\"m7-preindexed-first-viewport\""
              << ",\"boundary\":\"open-plus-first-layout-v1\""
              << ",\"profile\":\"" << device_frame_profile_name(*profile) << "\""
              << ",\"milliseconds\":" << elapsed_ms(started, finished)
              << ",\"fragments\":" << result.fragments.size()
              << ",\"used_checkpoint\":true}\n";
    return 0;
}

int run_warm_search(int argc, char** argv) {
    if (argc != 7) {
        std::cerr
            << "Usage: zevryon-m7-native-probe warm-search <store-dir>"
               " <sample-output> <trials> <query-utf8>"
               " open-once-one-warmup-v1\n";
        return 2;
    }
    const std::filesystem::path store_root(argv[2]);
    const std::filesystem::path sample_output(argv[3]);
    const auto trials = parse_number<std::size_t>(argv[4]);
    const std::string query(argv[5]);
    const std::string_view boundary(argv[6]);
    if (!trials || *trials < 5U || *trials > 1000U || query.empty() ||
        boundary != "open-once-one-warmup-v1") {
        std::cerr << "invalid M7 warm-search probe arguments\n";
        return 2;
    }

    StoreReader reader(store_root);
    std::string error;
    if (!reader.open(&error)) {
        std::cerr << "M7 warm-search store open failed: " << error << '\n';
        return 1;
    }
    SearchExecutionStats warm_stats;
    const auto warm_hits = reader.find_bounded(query, 1U, {}, &warm_stats, &error);
    if (!error.empty() || warm_hits.empty()) {
        std::cerr << "M7 warm-search warmup did not find the canonical query";
        if (!error.empty()) {
            std::cerr << ": " << error;
        }
        std::cerr << '\n';
        return 1;
    }

    std::ofstream output(sample_output, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "cannot open M7 warm-search sample output\n";
        return 1;
    }
    output << std::fixed << std::setprecision(9);
    std::uint64_t exact_records_scanned = 0U;
    for (std::size_t trial = 0U; trial < *trials; ++trial) {
        SearchExecutionStats stats;
        error.clear();
        const auto started = std::chrono::steady_clock::now();
        const auto hits = reader.find_bounded(query, 1U, {}, &stats, &error);
        const auto finished = std::chrono::steady_clock::now();
        if (!error.empty() || hits.empty()) {
            std::cerr << "M7 warm-search trial " << trial
                      << " did not find the canonical query";
            if (!error.empty()) {
                std::cerr << ": " << error;
            }
            std::cerr << '\n';
            return 1;
        }
        output << elapsed_ms(started, finished) << '\n';
        if (exact_records_scanned >
            std::numeric_limits<std::uint64_t>::max() - stats.exact_records_scanned) {
            exact_records_scanned = std::numeric_limits<std::uint64_t>::max();
        } else {
            exact_records_scanned += stats.exact_records_scanned;
        }
    }
    output.flush();
    if (!output) {
        std::cerr << "cannot finalize M7 warm-search sample output\n";
        return 1;
    }

    std::cout << "{\"operation\":\"m7-warm-exact-search\""
              << ",\"boundary\":\"open-once-one-warmup-v1\""
              << ",\"trials\":" << *trials
              << ",\"query_bytes\":" << query.size()
              << ",\"exact_records_scanned\":" << exact_records_scanned
              << "}\n";
    return 0;
}

void usage() {
    std::cerr
        << "Usage:\n"
        << "  zevryon-m7-native-probe preindexed <store-dir> <profile>"
           " <width-px> <height-px> <overscan-px> <max-fragments>"
           " open-plus-first-layout-v1\n"
        << "  zevryon-m7-native-probe warm-search <store-dir> <sample-output>"
           " <trials> <query-utf8> open-once-one-warmup-v1\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string_view operation(argv[1]);
    if (operation == "preindexed") {
        return run_preindexed(argc, argv);
    }
    if (operation == "warm-search") {
        return run_warm_search(argc, argv);
    }
    usage();
    return 2;
}
