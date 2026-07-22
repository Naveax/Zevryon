#include "zenith_hot_scroll.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
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

std::uint64_t bounded_add(
    std::uint64_t base,
    std::uint64_t delta,
    std::uint64_t maximum) noexcept {
    if (base >= maximum || delta >= maximum - base) {
        return maximum;
    }
    return base + delta;
}

struct ProfileResult {
    double p50_ms{0.0};
    double p95_ms{0.0};
    double p99_ms{0.0};
    double maximum_ms{0.0};
    std::uint64_t total_source_bytes_read{0};
    std::uint64_t maximum_source_bytes_read{0};
    std::uint64_t zero_source_read_queries{0};
    std::uint64_t checkpoint_cache_hits{0};
    std::uint64_t checkpoint_cache_misses{0};
    std::uint64_t source_window_cache_hits{0};
    std::uint64_t source_window_cache_misses{0};
};

std::uint64_t percentile_ns(std::vector<std::uint64_t> samples, std::size_t numerator) {
    std::sort(samples.begin(), samples.end());
    const std::size_t scaled = samples.size() * numerator;
    const std::size_t index = scaled == 0U ? 0U : (scaled - 1U) / 100U;
    return samples[std::min(index, samples.size() - 1U)];
}

double ns_to_ms(std::uint64_t value) {
    return static_cast<double>(value) / 1000000.0;
}

bool run_profile(
    const std::filesystem::path& store,
    zevryon::massivedoc::LayoutConfig config,
    const std::vector<std::uint64_t>& positions_q8,
    std::uint32_t width_q8,
    std::uint64_t height_q8,
    std::uint64_t overscan_q8,
    std::size_t max_fragments,
    bool warm_source_window,
    ProfileResult* profile,
    zevryon::massivedoc::ZenithHotScrollStats* session_stats,
    std::string* error) {
    if (profile == nullptr || session_stats == nullptr || error == nullptr ||
        positions_q8.empty()) {
        return false;
    }
    zevryon::massivedoc::ZenithHotScrollSession session(store, config);
    if (!session.open(error)) {
        return false;
    }

    zevryon::massivedoc::LayoutWindowResult warm;
    bool used_checkpoint = false;
    if (!session.layout(
            positions_q8.front(),
            width_q8,
            height_q8,
            overscan_q8,
            max_fragments,
            &warm,
            &used_checkpoint,
            error) ||
        !used_checkpoint || warm.fragments.empty()) {
        if (error->empty()) {
            *error = "hot-scroll warmup did not use checkpoints";
        }
        return false;
    }
    if (!warm_source_window) {
        session.clear_source_window_cache();
    }

    std::vector<std::uint64_t> latency_ns;
    latency_ns.reserve(positions_q8.size());
    ProfileResult output;
    for (const std::uint64_t position_q8 : positions_q8) {
        zevryon::massivedoc::LayoutWindowResult result;
        used_checkpoint = false;
        const auto started = std::chrono::steady_clock::now();
        if (!session.layout(
                position_q8,
                width_q8,
                height_q8,
                overscan_q8,
                max_fragments,
                &result,
                &used_checkpoint,
                error)) {
            return false;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started);
        if (!used_checkpoint || result.fragments.empty()) {
            *error = "hot-scroll query escaped checkpoint path";
            return false;
        }
        latency_ns.push_back(static_cast<std::uint64_t>(elapsed.count()));
        output.total_source_bytes_read += result.source_bytes_read;
        output.maximum_source_bytes_read = std::max(
            output.maximum_source_bytes_read, result.source_bytes_read);
        if (result.source_bytes_read == 0U) {
            ++output.zero_source_read_queries;
        }
        output.checkpoint_cache_hits += result.checkpoint_cache_hits;
        output.checkpoint_cache_misses += result.checkpoint_cache_misses;
        output.source_window_cache_hits += result.source_window_cache_hits;
        output.source_window_cache_misses += result.source_window_cache_misses;
    }

    output.p50_ms = ns_to_ms(percentile_ns(latency_ns, 50U));
    output.p95_ms = ns_to_ms(percentile_ns(latency_ns, 95U));
    output.p99_ms = ns_to_ms(percentile_ns(latency_ns, 99U));
    output.maximum_ms = ns_to_ms(*std::max_element(latency_ns.begin(), latency_ns.end()));
    *profile = output;
    *session_stats = session.stats();
    return true;
}

void write_profile_json(const ProfileResult& profile) {
    std::cout << "{\"p50_ms\":" << profile.p50_ms
              << ",\"p95_ms\":" << profile.p95_ms
              << ",\"p99_ms\":" << profile.p99_ms
              << ",\"maximum_ms\":" << profile.maximum_ms
              << ",\"total_source_bytes_read\":" << profile.total_source_bytes_read
              << ",\"maximum_source_bytes_read\":" << profile.maximum_source_bytes_read
              << ",\"zero_source_read_queries\":" << profile.zero_source_read_queries
              << ",\"checkpoint_cache_hits\":" << profile.checkpoint_cache_hits
              << ",\"checkpoint_cache_misses\":" << profile.checkpoint_cache_misses
              << ",\"source_window_cache_hits\":" << profile.source_window_cache_hits
              << ",\"source_window_cache_misses\":" << profile.source_window_cache_misses
              << '}';
}

void usage() {
    std::cerr
        << "Usage: zevryon-zenith-hot <store-dir> <center-scroll-y-px> <width-px>"
           " <height-px> <queries> [overscan-px] [max-fragments] [stride-kib]"
           " [random-radius-px]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 6 || argc > 10) {
        usage();
        return 2;
    }
    const auto center_q8 = pixels_to_q8(argv[2]);
    const auto width_q8 = pixels_to_q8(argv[3]);
    const auto height_q8 = pixels_to_q8(argv[4]);
    const auto queries = parse_number<std::size_t>(argv[5]);
    const auto overscan_q8 =
        argc >= 7 ? pixels_to_q8(argv[6])
                  : std::optional<std::uint64_t>{720U * 256U};
    const auto max_fragments =
        argc >= 8 ? parse_number<std::size_t>(argv[7])
                  : std::optional<std::size_t>{512U};
    const auto stride_kib =
        argc >= 9 ? parse_number<std::uint32_t>(argv[8])
                  : std::optional<std::uint32_t>{16U};
    const auto radius_q8 =
        argc == 10 ? pixels_to_q8(argv[9])
                   : std::optional<std::uint64_t>{1000000ULL * 256ULL};
    if (!center_q8 || !width_q8 || *width_q8 == 0U ||
        *width_q8 > std::numeric_limits<std::uint32_t>::max() || !height_q8 ||
        *height_q8 == 0U || !queries || *queries < 3U || !overscan_q8 ||
        !max_fragments || *max_fragments == 0U || !stride_kib ||
        *stride_kib == 0U ||
        *stride_kib > std::numeric_limits<std::uint32_t>::max() / 1024U ||
        !radius_q8) {
        std::cerr << "invalid hot-scroll benchmark arguments\n";
        return 2;
    }

    zevryon::massivedoc::LayoutConfig config;
    config.checkpoint_stride_bytes = *stride_kib * 1024U;
    std::string error;
    zevryon::massivedoc::ZenithHotScrollSession bounds_session(argv[1], config);
    if (!bounds_session.open(&error)) {
        std::cerr << "hot-scroll bounds open failed: " << error << '\n';
        return 1;
    }
    const std::uint64_t total_height_q8 = bounds_session.total_height_q8();
    const std::uint64_t max_scroll_q8 = total_height_q8 > *height_q8
                                            ? total_height_q8 - *height_q8
                                            : 0U;
    const std::uint64_t bounded_center_q8 = std::min(*center_q8, max_scroll_q8);

    std::vector<std::uint64_t> random_positions;
    random_positions.reserve(*queries);
    std::uint64_t state = 0x5a455652594f4eULL;
    const std::uint64_t radius = std::min(*radius_q8, max_scroll_q8);
    for (std::size_t index = 0U; index < *queries; ++index) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::uint64_t span = radius > (std::numeric_limits<std::uint64_t>::max() - 1U) / 2U
                                       ? std::numeric_limits<std::uint64_t>::max()
                                       : radius * 2U + 1U;
        const std::uint64_t sample = span == 0U ? 0U : state % span;
        std::uint64_t position = bounded_center_q8;
        if (sample <= radius) {
            const std::uint64_t delta = radius - sample;
            position = position >= delta ? position - delta : 0U;
        } else {
            position = bounded_add(position, sample - radius, max_scroll_q8);
        }
        random_positions.push_back(position);
    }

    std::vector<std::uint64_t> adjacent_positions;
    adjacent_positions.reserve(*queries);
    constexpr std::uint64_t kAdjacentStepQ8 = 18U * 256U;
    for (std::size_t index = 0U; index < *queries; ++index) {
        const std::uint64_t index_u64 = static_cast<std::uint64_t>(index);
        const std::uint64_t delta = index_u64 >
                                            std::numeric_limits<std::uint64_t>::max() /
                                                kAdjacentStepQ8
                                        ? std::numeric_limits<std::uint64_t>::max()
                                        : index_u64 * kAdjacentStepQ8;
        adjacent_positions.push_back(
            bounded_add(bounded_center_q8, delta, max_scroll_q8));
    }

    ProfileResult random_profile;
    ProfileResult adjacent_profile;
    zevryon::massivedoc::ZenithHotScrollStats random_stats;
    zevryon::massivedoc::ZenithHotScrollStats adjacent_stats;
    if (!run_profile(
            argv[1],
            config,
            random_positions,
            static_cast<std::uint32_t>(*width_q8),
            *height_q8,
            *overscan_q8,
            *max_fragments,
            false,
            &random_profile,
            &random_stats,
            &error)) {
        std::cerr << "random hot-scroll benchmark failed: " << error << '\n';
        return 1;
    }
    if (!run_profile(
            argv[1],
            config,
            adjacent_positions,
            static_cast<std::uint32_t>(*width_q8),
            *height_q8,
            *overscan_q8,
            *max_fragments,
            true,
            &adjacent_profile,
            &adjacent_stats,
            &error)) {
        std::cerr << "adjacent hot-scroll benchmark failed: " << error << '\n';
        return 1;
    }

    std::cout << std::fixed << std::setprecision(6)
              << "{\"operation\":\"zenith-hot-scroll\",\"queries\":" << *queries
              << ",\"stride_bytes\":" << config.checkpoint_stride_bytes
              << ",\"checkpoint_cache_budget_bytes\":"
              << config.max_checkpoint_cache_bytes
              << ",\"source_window_cache_budget_bytes\":"
              << config.max_source_window_cache_bytes
              << ",\"random\":";
    write_profile_json(random_profile);
    std::cout << ",\"adjacent\":";
    write_profile_json(adjacent_profile);
    std::cout << ",\"random_session\":"
              << zevryon::massivedoc::zenith_hot_scroll_stats_json(random_stats)
              << ",\"adjacent_session\":"
              << zevryon::massivedoc::zenith_hot_scroll_stats_json(adjacent_stats)
              << "}\n";
    return 0;
}
