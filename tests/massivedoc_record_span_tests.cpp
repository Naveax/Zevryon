#include "massivedoc_store.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using zevryon::massivedoc::CorpusMetadata;
using zevryon::massivedoc::StoreReadConfig;
using zevryon::massivedoc::StoreReader;
using zevryon::massivedoc::StoreStats;
using zevryon::massivedoc::StoreWriter;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: MassiveDoc record span: " << message << '\n';
        return false;
    }
    return true;
}

std::filesystem::path unique_root(std::string_view name) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (std::string("zevryon-") + std::string(name) + "-" + std::to_string(tick));
}

struct RootCleanup {
    explicit RootCleanup(std::filesystem::path value) : root(std::move(value)) {}
    ~RootCleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    RootCleanup(const RootCleanup&) = delete;
    RootCleanup& operator=(const RootCleanup&) = delete;

    std::filesystem::path root;
};

std::span<const std::byte> bytes(std::string_view value) {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(value.data()), value.size());
}

bool build_store(const std::filesystem::path& root, std::string* error) {
    StoreWriter writer(root);
    constexpr std::string_view first = "ab";
    constexpr std::string_view second = "cdef";
    constexpr std::string_view third = "ghi";
    if (!writer.append(1001U, bytes(first), error) ||
        !writer.append(1002U, bytes(second), error) ||
        !writer.append(1003U, bytes(third), error)) {
        return false;
    }

    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = first.size() + second.size() + third.size();
    metadata.logical_records = 3U;
    metadata.logical_nodes = 1U;
    metadata.largest_record_bytes = second.size();
    StoreStats stats;
    return writer.finalize(metadata, &stats, error);
}

std::string as_text(const std::vector<std::byte>& value) {
    return std::string(
        reinterpret_cast<const char*>(value.data()), value.size());
}

bool test_cross_record_stream_and_window_independence() {
    const std::filesystem::path root = unique_root("record-span-cross");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_store(root, &error), error)) {
        return false;
    }

    for (const std::size_t window : {1U, 2U, 7U}) {
        StoreReadConfig config;
        config.io_window_bytes = window;
        StoreReader reader(root, config);
        if (!require(reader.open(&error), error)) {
            return false;
        }

        std::vector<std::byte> output;
        if (!require(
                reader.read_record_span(
                    0U,
                    1U,
                    6U,
                    [&](std::span<const std::byte> chunk) {
                        output.insert(output.end(), chunk.begin(), chunk.end());
                        return true;
                    },
                    &error),
                error) ||
            !require(as_text(output) == "bcdefg", "cross-record bytes are exact")) {
            return false;
        }
    }
    return true;
}

bool test_zero_length_and_record_end_start() {
    const std::filesystem::path root = unique_root("record-span-zero");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_store(root, &error), error)) {
        return false;
    }
    StoreReadConfig config;
    config.io_window_bytes = 1U;
    StoreReader reader(root, config);
    if (!require(reader.open(&error), error)) {
        return false;
    }

    std::uint64_t callbacks = 0U;
    if (!require(
            reader.read_record_span(
                0U,
                2U,
                0U,
                [&](std::span<const std::byte>) {
                    ++callbacks;
                    return true;
                },
                &error),
            error) ||
        !require(callbacks == 0U, "zero-length span emits no bytes")) {
        return false;
    }

    std::vector<std::byte> output;
    if (!require(
            reader.read_record_span(
                0U,
                2U,
                3U,
                [&](std::span<const std::byte> chunk) {
                    output.insert(output.end(), chunk.begin(), chunk.end());
                    return true;
                },
                &error),
            error) ||
        !require(as_text(output) == "cde", "end-of-record start continues at next record")) {
        return false;
    }
    return true;
}

bool test_invalid_bounds_fail_closed() {
    const std::filesystem::path root = unique_root("record-span-bounds");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_store(root, &error), error)) {
        return false;
    }
    StoreReader reader(root);
    if (!require(reader.open(&error), error)) {
        return false;
    }
    const auto consume = [](std::span<const std::byte>) { return true; };

    if (!require(
            !reader.read_record_span(3U, 0U, 1U, consume, &error),
            "start record outside corpus rejected") ||
        !require(
            !reader.read_record_span(0U, 3U, 1U, consume, &error),
            "start offset outside first record rejected") ||
        !require(
            !reader.read_record_span(2U, 0U, 4U, consume, &error),
            "span escaping final record rejected")) {
        return false;
    }
    return true;
}

bool test_consumer_early_stop_is_success() {
    const std::filesystem::path root = unique_root("record-span-stop");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_store(root, &error), error)) {
        return false;
    }
    StoreReadConfig config;
    config.io_window_bytes = 1U;
    StoreReader reader(root, config);
    if (!require(reader.open(&error), error)) {
        return false;
    }

    std::vector<std::byte> output;
    if (!require(
            reader.read_record_span(
                0U,
                1U,
                8U,
                [&](std::span<const std::byte> chunk) {
                    output.insert(output.end(), chunk.begin(), chunk.end());
                    return output.size() < 2U;
                },
                &error),
            "consumer early stop is successful") ||
        !require(as_text(output) == "bc", "early stop emits only requested prefix")) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!test_cross_record_stream_and_window_independence() ||
        !test_zero_length_and_record_end_start() ||
        !test_invalid_bounds_fail_closed() ||
        !test_consumer_early_stop_is_success()) {
        return 1;
    }
    std::cout << "MassiveDoc record span tests passed\n";
    return 0;
}
