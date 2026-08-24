#include "device_frame_profile.hpp"
#include "frame_latency_sample_collector.hpp"
#include "shared_record_length_authority.hpp"
#include "shared_source_prefetch_pool.hpp"
#include "zenith_hot_scroll.hpp"
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

constexpr std::size_t kMaximumProbeSamples = 1'000'000U;

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

std::int64_t signed_velocity(
    std::int8_t direction,
    std::uint64_t step_q8,
    std::uint32_t frame_budget_us) noexcept {
    if ((direction != 1 && direction != -1) || frame_budget_us == 0U) {
        return 0;
    }
    const std::uint64_t maximum =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const std::uint64_t scaled = step_q8 > maximum / 1'000'000U
                                     ? maximum
                                     : step_q8 * 1'000'000U;
    const std::uint64_t magnitude = scaled / frame_budget_us;
    const std::int64_t signed_magnitude = static_cast<std::int64_t>(magnitude);
    return direction > 0 ? signed_magnitude : -signed_magnitude;
}

bool update_position(
    std::uint64_t step_q8,
    std::uint64_t maximum_scroll_q8,
    std::uint64_t* position_q8,
    std::int8_t* direction) noexcept {
    if (position_q8 == nullptr || direction == nullptr || step_q8 == 0U ||
        maximum_scroll_q8 == 0U || (*direction != 1 && *direction != -1)) {
        return false;
    }
    const std::int8_t before = *direction;
    if (*direction > 0) {
        const std::uint64_t remaining = maximum_scroll_q8 - *position_q8;
        if (step_q8 >= remaining) {
            *position_q8 = maximum_scroll_q8;
            *direction = -1;
        } else {
            *position_q8 += step_q8;
        }
    } else if (step_q8 >= *position_q8) {
        *position_q8 = 0U;
        *direction = 1;
    } else {
        *position_q8 -= step_q8;
    }
    return before != *direction;
}

void usage() {
    std::cerr
        << "Usage: zevryon-zenith-frame-probe <store-dir> <profile> <sample-output>"
           " <samples> [warmup=120] [width-px=1440] [height-px=900]"
           " [overscan-px=720] [max-fragments=512] [step-px=18]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 5 || argc > 11) {
        usage();
        return 2;
    }

    const auto profile = parse_profile(argv[2]);
    const auto samples = parse_number<std::size_t>(argv[4]);
    const auto warmup = argc >= 6 ? parse_number<std::size_t>(argv[5])
                                  : std::optional<std::size_t>{120U};
    const auto width_q8 = argc >= 7 ? pixels_to_q8(argv[6])
                                    : std::optional<std::uint64_t>{1440U * 256U};
    const auto height_q8 = argc >= 8 ? pixels_to_q8(argv[7])
                                     : std::optional<std::uint64_t>{900U * 256U};
    const auto overscan_q8 = argc >= 9 ? pixels_to_q8(argv[8])
                                       : std::optional<std::uint64_t>{720U * 256U};
    const auto max_fragments = argc >= 10 ? parse_number<std::size_t>(argv[9])
                                          : std::optional<std::size_t>{512U};
    const auto step_q8 = argc == 11 ? pixels_to_q8(argv[10])
                                    : std::optional<std::uint64_t>{18U * 256U};

    if (!profile || !samples || *samples == 0U || *samples > kMaximumProbeSamples ||
        !warmup || *warmup > kMaximumProbeSamples || !width_q8 || *width_q8 == 0U ||
        *width_q8 > std::numeric_limits<std::uint32_t>::max() || !height_q8 ||
        *height_q8 == 0U || !overscan_q8 || !max_fragments || *max_fragments == 0U ||
        !step_q8 || *step_q8 == 0U || *warmup > kMaximumProbeSamples - *samples) {
        std::cerr << "invalid frame probe arguments\n";
        return 2;
    }

    const std::filesystem::path store_root(argv[1]);
    const std::filesystem::path output_path(argv[3]);
    SharedRecordLengthAuthority record_lengths;
    SharedSourcePrefetchPoolConfig pool_config;
    pool_config.record_length_authority = &record_lengths;
    SharedSourcePrefetchPool pool(pool_config);

    ZenithTabRuntimeConfig runtime_config = make_zenith_tab_runtime_config(*profile);
    runtime_config.record_length_authority = &record_lengths;
    if (!runtime_config.valid() || !pool.valid()) {
        std::cerr << "frame probe configuration is invalid\n";
        return 1;
    }

    std::uint64_t maximum_scroll_q8 = 0U;
    {
        ZenithHotScrollSession bounds(store_root, runtime_config.layout);
        std::string error;
        if (!bounds.open(&error)) {
            std::cerr << "frame probe bounds open failed: " << error << '\n';
            return 1;
        }
        const std::uint64_t total_height_q8 = bounds.total_height_q8();
        maximum_scroll_q8 = total_height_q8 > *height_q8
                                ? total_height_q8 - *height_q8
                                : 0U;
    }
    if (maximum_scroll_q8 == 0U) {
        std::cerr << "frame probe workload has no scrollable range\n";
        return 1;
    }

    FrameLatencySampleCollector collector({*warmup, *samples});
    if (!collector.valid()) {
        std::cerr << "frame probe collector configuration is invalid\n";
        return 1;
    }

    ZenithTabRuntime runtime(store_root, &pool, 1U, runtime_config);
    std::string error;
    if (!runtime.open(&error)) {
        std::cerr << "frame probe runtime open failed: " << error << '\n';
        return 1;
    }

    std::uint64_t position_q8 = 0U;
    std::int8_t direction = 1;
    const std::uint32_t frame_budget_us = runtime_config.frame_budget.frame_budget_us;
    if (!runtime.set_activity(
            FrameVisibility::Visible,
            FramePressure::Normal,
            signed_velocity(direction, *step_q8, frame_budget_us),
            &error)) {
        std::cerr << "frame probe activity setup failed: " << error << '\n';
        return 1;
    }

    const std::size_t total_observations = *warmup + *samples;
    for (std::size_t index = 0U; index < total_observations; ++index) {
        LayoutWindowResult result;
        bool used_checkpoint = false;
        const auto started = std::chrono::steady_clock::now();
        const bool succeeded = runtime.layout(
            position_q8,
            static_cast<std::uint32_t>(*width_q8),
            *height_q8,
            *overscan_q8,
            *max_fragments,
            &result,
            &used_checkpoint,
            &error);
        const auto finished = std::chrono::steady_clock::now();
        if (!succeeded) {
            std::cerr << "frame probe layout failed at observation " << index
                      << ": " << error << '\n';
            return 1;
        }
        if (!used_checkpoint || result.fragments.empty()) {
            std::cerr << "frame probe escaped checkpoint path at observation "
                      << index << '\n';
            return 1;
        }
        static_cast<void>(collector.observe(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started)));

        if (update_position(*step_q8, maximum_scroll_q8, &position_q8, &direction)) {
            if (!runtime.set_activity(
                    FrameVisibility::Visible,
                    FramePressure::Normal,
                    signed_velocity(direction, *step_q8, frame_budget_us),
                    &error)) {
                std::cerr << "frame probe direction update failed: " << error << '\n';
                return 1;
            }
        }
    }

    const auto collector_status = collector.status();
    if (collector_status.recorded != static_cast<std::uint64_t>(*samples) ||
        collector.samples_ns().size() != *samples) {
        std::cerr << "frame probe did not retain the requested sample count\n";
        return 1;
    }

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "frame probe cannot open sample output\n";
        return 1;
    }
    output << std::fixed << std::setprecision(9);
    for (const std::uint64_t sample_ns : collector.samples_ns()) {
        output << static_cast<double>(sample_ns) / 1'000'000.0 << '\n';
    }
    output.flush();
    if (!output) {
        std::cerr << "frame probe cannot finalize sample output\n";
        return 1;
    }

    static_cast<void>(pool.wait_idle_for(std::chrono::seconds(5)));
    const SharedSourcePrefetchPoolStatus pool_status = pool.status();
    const ZenithTabRuntimeStats& runtime_stats = runtime.stats();
    std::cout << "{\"operation\":\"zenith-tab-runtime-frame-probe\""
              << ",\"profile\":\"" << device_frame_profile_name(*profile) << "\""
              << ",\"frame_budget_us\":" << frame_budget_us
              << ",\"warmup_samples\":" << *warmup
              << ",\"recorded_samples\":" << *samples
              << ",\"visible_layouts\":" << runtime_stats.visible_layouts
              << ",\"frame_overruns\":" << runtime_stats.visible_frame_overruns
              << ",\"prefetch_accepts\":" << runtime_stats.prefetch_schedule_accepts
              << ",\"pool_thread_starts\":" << pool_status.thread_starts
              << ",\"pool_ready_peak_bytes\":" << pool_status.ready_peak_bytes
              << "}\n";
    return 0;
}
