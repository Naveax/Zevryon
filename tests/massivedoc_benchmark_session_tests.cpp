#include "massivedoc_benchmark_session.hpp"
#include "massivedoc_store.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

std::filesystem::path temp_root(std::string_view name) {
    std::mt19937_64 random(0x4d3742454e434853ULL);
    const auto root = std::filesystem::temp_directory_path() /
                      (std::string("zevryon-") + std::string(name) + "-" +
                       std::to_string(random()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error, "cannot clear benchmark-session test root");
    return root;
}

void cleanup(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error, "benchmark-session test cleanup failed");
}

std::vector<std::byte> fixture_payload(std::size_t bytes) {
    const std::string pattern = "alpha beta gamma\tdelta epsilon\n";
    std::vector<std::byte> output;
    output.reserve(bytes);
    while (output.size() < bytes) {
        const std::size_t remaining = bytes - output.size();
        const std::size_t copy_bytes = std::min(remaining, pattern.size());
        const auto* begin = reinterpret_cast<const std::byte*>(pattern.data());
        output.insert(
            output.end(),
            begin,
            begin + static_cast<std::ptrdiff_t>(copy_bytes));
    }
    return output;
}

void create_store(const std::filesystem::path& root, std::size_t payload_bytes) {
    zevryon::massivedoc::StoreConfig config;
    config.segment_bytes = 4096U;
    zevryon::massivedoc::StoreWriter writer(root, config);
    std::string error;
    const auto payload = fixture_payload(payload_bytes);
    require(writer.append(1001U, payload, &error), error);
    zevryon::massivedoc::CorpusMetadata metadata;
    metadata.logical_utf8_bytes = static_cast<std::uint64_t>(payload.size());
    metadata.logical_records = 1U;
    metadata.logical_nodes = 1U;
    metadata.largest_record_bytes = static_cast<std::uint64_t>(payload.size());
    zevryon::massivedoc::StoreStats stats;
    require(writer.finalize(metadata, &stats, &error), error);
    require(stats.corpus.logical_records == 1U, "benchmark fixture record count mismatch");
}

void test_query_generator_matches_scenario_lcg() {
    using zevryon::massivedoc::BenchmarkSessionMode;
    using zevryon::massivedoc::DeterministicBenchmarkQueryGenerator;

    DeterministicBenchmarkQueryGenerator virtualized(
        BenchmarkSessionMode::Virtualized,
        1000U,
        100U);
    const std::uint64_t expected_virtual[] = {672U, 468U, 666U, 500U, 799U};
    for (const std::uint64_t expected : expected_virtual) {
        require(virtualized.next() == expected, "virtualized LCG coordinate drifted");
    }

    DeterministicBenchmarkQueryGenerator native(
        BenchmarkSessionMode::NativeDom,
        1000U,
        100U);
    const std::uint64_t expected_native[] = {
        747581U,
        520360U,
        740833U,
        555867U,
        888134U,
    };
    for (const std::uint64_t expected : expected_native) {
        require(native.next() == expected, "native LCG coordinate drifted");
    }
}

void test_virtualized_session_reuses_open_store() {
    const auto root = temp_root("benchmark-session-virtual");
    constexpr std::size_t kPayloadBytes = 256U * 1024U;
    create_store(root, kPayloadBytes);

    {
        zevryon::massivedoc::BenchmarkSessionConfig config;
        config.store_root = root;
        config.record_index = 0U;
        config.payload_bytes = kPayloadBytes;
        config.virtual_slice_bytes = 4096U;

        zevryon::massivedoc::MassiveDocBenchmarkSession session;
        zevryon::massivedoc::BenchmarkSessionReady ready;
        std::string error;
        require(
            session.open(
                zevryon::massivedoc::BenchmarkSessionMode::Virtualized,
                config,
                &ready,
                &error),
            error);
        require(session.is_open(), "virtualized session did not remain open");
        require(ready.payload_bytes == kPayloadBytes, "virtualized ready payload drifted");
        require(ready.native_checkpoint_bytes == 0U, "virtualized session published native checkpoint bytes");

        zevryon::massivedoc::BenchmarkQueryReceipt first;
        require(session.query(0U, &first, &error), error);
        require(first.source_bytes_read == 4096U, "virtualized slice read size drifted");
        require(first.rendered_height_q8 > 0U, "virtualized layout produced zero height");
        require(first.milliseconds >= 0.0, "virtualized query timing became negative");

        zevryon::massivedoc::BenchmarkQueryReceipt second;
        require(session.query(8192U, &second, &error), error);
        require(second.source_bytes_read == 4096U, "second virtualized slice read size drifted");
        require(second.rendered_height_q8 > 0U, "second virtualized layout produced zero height");

        zevryon::massivedoc::BenchmarkQueryReceipt invalid;
        require(
            !session.query(static_cast<std::uint64_t>(kPayloadBytes), &invalid, &error),
            "out-of-range virtualized query was accepted");
    }

    cleanup(root);
}

void test_native_session_reuses_one_record_checkpoint() {
    const auto root = temp_root("benchmark-session-native");
    constexpr std::size_t kPayloadBytes = 256U * 1024U;
    create_store(root, kPayloadBytes);

    {
        zevryon::massivedoc::BenchmarkSessionConfig config;
        config.store_root = root;
        config.record_index = 0U;
        config.payload_bytes = kPayloadBytes;
        config.virtual_slice_bytes = 4096U;
        config.max_fragments = 512U;
        config.checkpoint_stride_bytes = 4096U;

        zevryon::massivedoc::MassiveDocBenchmarkSession session;
        zevryon::massivedoc::BenchmarkSessionReady ready;
        std::string error;
        require(
            session.open(
                zevryon::massivedoc::BenchmarkSessionMode::NativeDom,
                config,
                &ready,
                &error),
            error);
        require(ready.native_total_height_q8 > 0U, "native setup produced zero height");
        require(ready.native_checkpoint_bytes > 0U, "native setup produced no checkpoint evidence");

        zevryon::massivedoc::BenchmarkQueryReceipt first;
        require(session.query(0U, &first, &error), error);
        require(first.fragment_count > 0U, "native top query returned no fragments");
        require(first.milliseconds >= 0.0, "native top query timing became negative");

        zevryon::massivedoc::BenchmarkQueryReceipt middle;
        require(session.query(520360U, &middle, &error), error);
        require(middle.fragment_count > 0U, "native middle query returned no fragments");
        require(middle.rendered_height_q8 > 0U, "native total height receipt was empty");
        require(
            middle.checkpoint_source_offset > 0U,
            "native middle query did not reuse an interior checkpoint");

        zevryon::massivedoc::BenchmarkQueryReceipt invalid;
        require(
            !session.query(1000000U, &invalid, &error),
            "out-of-range native coordinate was accepted");
    }

    cleanup(root);
}

} // namespace

int main() {
    test_query_generator_matches_scenario_lcg();
    test_virtualized_session_reuses_open_store();
    test_native_session_reuses_one_record_checkpoint();
    std::cout << "Zevryon MassiveDoc persistent benchmark-session tests passed\n";
    return 0;
}
