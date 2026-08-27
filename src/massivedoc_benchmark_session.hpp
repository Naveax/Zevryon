#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace zevryon::massivedoc {

enum class BenchmarkSessionMode : std::uint8_t {
    Virtualized = 0,
    NativeDom,
};

struct BenchmarkSessionConfig {
    std::filesystem::path store_root;
    std::uint64_t record_index{0U};
    std::uint64_t payload_bytes{0U};
    std::size_t virtual_slice_bytes{128U * 1024U};
    std::uint32_t viewport_width_px{800U};
    std::uint32_t viewport_height_px{720U};
    std::size_t max_fragments{512U};
    std::uint32_t checkpoint_stride_bytes{64U * 1024U};
};

struct BenchmarkSessionReady {
    BenchmarkSessionMode mode{BenchmarkSessionMode::Virtualized};
    std::uint64_t payload_bytes{0U};
    std::uint64_t native_total_height_q8{0U};
    std::uint64_t native_checkpoint_bytes{0U};
};

struct BenchmarkSyntheticStoreReady {
    std::uint64_t record_index{0U};
    std::uint64_t payload_bytes{0U};
    std::uint64_t physical_bytes{0U};
    std::string payload_sha256;
};

struct BenchmarkQueryReceipt {
    std::uint64_t coordinate{0U};
    double milliseconds{0.0};
    std::uint64_t source_bytes_read{0U};
    std::uint64_t rendered_height_q8{0U};
    std::uint64_t checkpoint_source_offset{0U};
    std::size_t fragment_count{0U};
    bool truncated{false};
};

class DeterministicBenchmarkQueryGenerator final {
public:
    DeterministicBenchmarkQueryGenerator(
        BenchmarkSessionMode mode,
        std::uint64_t payload_bytes,
        std::size_t virtual_slice_bytes);

    std::uint64_t next() noexcept;

private:
    std::uint64_t maximum_{0U};
    std::uint32_t state_{0x243f6a88U};
};

class MassiveDocBenchmarkSession final {
public:
    MassiveDocBenchmarkSession();
    ~MassiveDocBenchmarkSession();

    MassiveDocBenchmarkSession(const MassiveDocBenchmarkSession&) = delete;
    MassiveDocBenchmarkSession& operator=(const MassiveDocBenchmarkSession&) = delete;
    MassiveDocBenchmarkSession(MassiveDocBenchmarkSession&&) = delete;
    MassiveDocBenchmarkSession& operator=(MassiveDocBenchmarkSession&&) = delete;

    bool open(
        BenchmarkSessionMode mode,
        BenchmarkSessionConfig config,
        BenchmarkSessionReady* ready,
        std::string* error);

    bool query(
        std::uint64_t coordinate,
        BenchmarkQueryReceipt* receipt,
        std::string* error);

    bool is_open() const noexcept;
    BenchmarkSessionMode mode() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool build_m7_synthetic_benchmark_store(
    const std::filesystem::path& store_root,
    std::uint64_t payload_bytes,
    BenchmarkSyntheticStoreReady* ready,
    std::string* error);

const char* benchmark_session_mode_name(BenchmarkSessionMode mode) noexcept;

} // namespace zevryon::massivedoc
