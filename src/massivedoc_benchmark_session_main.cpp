#include "massivedoc_benchmark_session.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

using zevryon::massivedoc::BenchmarkQueryReceipt;
using zevryon::massivedoc::BenchmarkSessionConfig;
using zevryon::massivedoc::BenchmarkSessionMode;
using zevryon::massivedoc::BenchmarkSessionReady;
using zevryon::massivedoc::BenchmarkSyntheticStoreReady;
using zevryon::massivedoc::DeterministicBenchmarkQueryGenerator;
using zevryon::massivedoc::MassiveDocBenchmarkSession;

constexpr std::string_view kM7SourceAuthority =
    "case-owned-m7-synthetic-single-record-v1";
constexpr std::string_view kLegacySourceAuthority =
    "prebuilt-store-diagnostic-v1";

template <typename T>
std::optional<T> parse_number(std::string_view text) {
    T value{};
    const auto parsed = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<BenchmarkSessionMode> parse_mode(std::string_view text) {
    if (text == "virtualized") {
        return BenchmarkSessionMode::Virtualized;
    }
    if (text == "native-dom") {
        return BenchmarkSessionMode::NativeDom;
    }
    return std::nullopt;
}

void usage() {
    std::cerr
        << "Usage:\n"
        << "  zevryon-massivedoc-benchmark-session "
        << "<virtualized|native-dom> <store-dir> <record-index> <payload-bytes> "
        << "<query-count> <warmup-count> <slice-bytes> <viewport-width-px> "
        << "<viewport-height-px> <max-fragments>\n"
        << "  zevryon-massivedoc-benchmark-session --m7-synthetic "
        << "<virtualized|native-dom> <case-root> <payload-bytes> <query-count> "
        << "<warmup-count> <slice-bytes> <viewport-width-px> "
        << "<viewport-height-px> <max-fragments>\n";
}

void print_query(
    BenchmarkSessionMode mode,
    std::size_t ordinal,
    const BenchmarkQueryReceipt& receipt) {
    std::cout << std::setprecision(17)
              << "{\"schema\":\"zevryon.massivedoc.persistent-benchmark-session.v1\","
              << "\"event\":\"query\","
              << "\"ordinal\":" << ordinal << ',';
    if (mode == BenchmarkSessionMode::Virtualized) {
        std::cout << "\"byte_offset\":" << receipt.coordinate << ',';
    } else {
        std::cout << "\"scroll_fraction_ppm\":" << receipt.coordinate << ',';
    }
    std::cout << "\"milliseconds\":" << receipt.milliseconds << ','
              << "\"source_bytes_read\":" << receipt.source_bytes_read << ','
              << "\"rendered_height_q8\":" << receipt.rendered_height_q8 << ','
              << "\"checkpoint_source_offset\":" << receipt.checkpoint_source_offset << ','
              << "\"fragment_count\":" << receipt.fragment_count << ','
              << "\"truncated\":" << (receipt.truncated ? "true" : "false")
              << "}\n"
              << std::flush;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 11) {
        usage();
        return 2;
    }

    const bool m7_synthetic = std::string_view(argv[1]) == "--m7-synthetic";
    const int mode_index = m7_synthetic ? 2 : 1;
    const auto mode = parse_mode(argv[mode_index]);
    const auto record_index = m7_synthetic
        ? std::optional<std::uint64_t>{0U}
        : parse_number<std::uint64_t>(argv[3]);
    const auto payload_bytes = parse_number<std::uint64_t>(argv[4]);
    const auto query_count = parse_number<std::size_t>(argv[5]);
    const auto warmup_count = parse_number<std::size_t>(argv[6]);
    const auto slice_bytes = parse_number<std::size_t>(argv[7]);
    const auto viewport_width = parse_number<std::uint32_t>(argv[8]);
    const auto viewport_height = parse_number<std::uint32_t>(argv[9]);
    const auto max_fragments = parse_number<std::size_t>(argv[10]);
    if (!mode || !record_index || !payload_bytes || *payload_bytes == 0U ||
        !query_count || *query_count == 0U || !warmup_count || !slice_bytes ||
        *slice_bytes == 0U || !viewport_width || *viewport_width == 0U ||
        !viewport_height || *viewport_height == 0U || !max_fragments ||
        *max_fragments == 0U) {
        std::cerr << "invalid persistent benchmark-session arguments\n";
        return 2;
    }

    std::filesystem::path store_root;
    BenchmarkSyntheticStoreReady synthetic_ready;
    if (m7_synthetic) {
        const std::filesystem::path case_root = argv[3];
        if (case_root.empty()) {
            std::cerr << "M7 synthetic case root cannot be empty\n";
            return 2;
        }
        store_root = case_root / "store";
    } else {
        store_root = argv[2];
    }

    const auto setup_started = std::chrono::steady_clock::now();
    std::string error;
    if (m7_synthetic &&
        !zevryon::massivedoc::build_m7_synthetic_benchmark_store(
            store_root,
            *payload_bytes,
            &synthetic_ready,
            &error)) {
        std::cerr << "M7 synthetic benchmark store build failed: " << error << '\n';
        return 1;
    }

    BenchmarkSessionConfig config;
    config.store_root = store_root;
    config.record_index = m7_synthetic ? synthetic_ready.record_index : *record_index;
    config.payload_bytes = *payload_bytes;
    config.virtual_slice_bytes = *slice_bytes;
    config.viewport_width_px = *viewport_width;
    config.viewport_height_px = *viewport_height;
    config.max_fragments = *max_fragments;

    MassiveDocBenchmarkSession session;
    BenchmarkSessionReady ready;
    if (!session.open(*mode, config, &ready, &error)) {
        std::cerr << "persistent benchmark session open failed: " << error << '\n';
        return 1;
    }

    DeterministicBenchmarkQueryGenerator generator(
        *mode,
        *payload_bytes,
        *slice_bytes);
    for (std::size_t warmup_index = 0U; warmup_index < *warmup_count; ++warmup_index) {
        BenchmarkQueryReceipt receipt;
        if (!session.query(generator.next(), &receipt, &error)) {
            std::cerr << "persistent benchmark warmup failed: " << error << '\n';
            return 1;
        }
    }

    const double internal_setup_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - setup_started).count();
    std::cout << std::setprecision(17)
              << "{\"schema\":\"zevryon.massivedoc.persistent-benchmark-session.v1\","
              << "\"event\":\"ready\","
              << "\"mode\":\""
              << zevryon::massivedoc::benchmark_session_mode_name(*mode) << "\","
              << "\"payload_bytes\":" << ready.payload_bytes << ','
              << "\"query_count\":" << *query_count << ','
              << "\"warmup_query_count\":" << *warmup_count << ','
              << "\"virtual_slice_bytes\":" << *slice_bytes << ','
              << "\"viewport_width_px\":" << *viewport_width << ','
              << "\"viewport_height_px\":" << *viewport_height << ','
              << "\"native_total_height_q8\":" << ready.native_total_height_q8 << ','
              << "\"native_checkpoint_bytes\":" << ready.native_checkpoint_bytes << ','
              << "\"internal_setup_seconds\":" << internal_setup_seconds << ','
              << "\"source_authority\":\""
              << (m7_synthetic ? kM7SourceAuthority : kLegacySourceAuthority)
              << "\","
              << "\"record_index\":" << config.record_index << ',';
    if (m7_synthetic) {
        std::cout << "\"store_payload_sha256\":\""
                  << synthetic_ready.payload_sha256 << "\","
                  << "\"store_physical_bytes\":" << synthetic_ready.physical_bytes << ',';
    } else {
        std::cout << "\"store_payload_sha256\":null,"
                  << "\"store_physical_bytes\":0,";
    }
    std::cout << "\"normalized_leadership_evidence\":false"
              << "}\n"
              << std::flush;

    for (std::size_t query_index = 0U; query_index < *query_count; ++query_index) {
        BenchmarkQueryReceipt receipt;
        if (!session.query(generator.next(), &receipt, &error)) {
            std::cerr << "persistent benchmark query failed: " << error << '\n';
            return 1;
        }
        print_query(*mode, query_index, receipt);
    }

    std::cout << "{\"schema\":\"zevryon.massivedoc.persistent-benchmark-session.v1\","
              << "\"event\":\"complete\","
              << "\"query_count\":" << *query_count << ','
              << "\"normalized_leadership_evidence\":false"
              << "}\n"
              << std::flush;
    return 0;
}
