#include "massivedoc_benchmark_session.hpp"
#include "zenith_process_memory_pressure.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using zevryon::massivedoc::BenchmarkQueryReceipt;
using zevryon::massivedoc::BenchmarkSessionConfig;
using zevryon::massivedoc::BenchmarkSessionMode;
using zevryon::massivedoc::BenchmarkSessionReady;
using zevryon::massivedoc::BenchmarkSyntheticStoreReady;
using zevryon::massivedoc::DeterministicBenchmarkQueryGenerator;
using zevryon::massivedoc::MassiveDocBenchmarkSession;
using zevryon::massivedoc::ZenithProcessMemorySnapshot;

constexpr std::uint64_t kCertificationSeconds = 86'400ULL;
constexpr std::uint64_t kDefaultSmokeSeconds = 3ULL;
constexpr std::uint64_t kDefaultPayloadBytes = 4ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMinimumPayloadBytes = 4'096ULL;
constexpr std::uint64_t kSmokeCheckpointMs = 1'000ULL;
constexpr std::uint64_t kCertificationCheckpointMs = 60'000ULL;
constexpr std::uint64_t kSmokeMemorySampleMs = 250ULL;
constexpr std::uint64_t kCertificationMemorySampleMs = 1'000ULL;
constexpr std::size_t kWarmupQueriesPerMode = 4U;

struct Config {
    std::filesystem::path work_dir;
    std::optional<std::filesystem::path> output;
    std::uint64_t duration_seconds{kDefaultSmokeSeconds};
    std::uint64_t payload_bytes{kDefaultPayloadBytes};
    bool certification{false};
};

struct SoakState {
    std::uint64_t virtualized_queries{0U};
    std::uint64_t native_queries{0U};
    std::uint64_t checkpoints{0U};
    std::uint64_t memory_samples{0U};
    std::uint64_t memory_snapshot_failures{0U};
    std::uint64_t query_failures{0U};
    std::uint64_t current_rss_bytes{0U};
    std::uint64_t peak_rss_bytes{0U};
    std::uint64_t rolling_digest{0U};
    double max_virtualized_query_ms{0.0};
    double max_native_query_ms{0.0};
};

bool parse_u64(std::string_view text, std::uint64_t* value) {
    if (value == nullptr || text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0U;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    *value = parsed;
    return true;
}

bool parse_args(int argc, char** argv, Config* config, std::string* error) {
    if (config == nullptr || error == nullptr) {
        return false;
    }
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg(argv[index]);
        if (arg == "--certification") {
            config->certification = true;
            continue;
        }
        if (arg == "--work-dir" || arg == "--output" ||
            arg == "--duration-seconds" || arg == "--payload-bytes") {
            if (index + 1 >= argc) {
                *error = std::string(arg) + " requires a value";
                return false;
            }
            const std::string_view value(argv[++index]);
            if (arg == "--work-dir") {
                config->work_dir = std::filesystem::path(std::string(value));
                continue;
            }
            if (arg == "--output") {
                config->output = std::filesystem::path(std::string(value));
                continue;
            }
            std::uint64_t parsed = 0U;
            if (!parse_u64(value, &parsed)) {
                *error = std::string(arg) + " must be an unsigned decimal integer";
                return false;
            }
            if (arg == "--duration-seconds") {
                config->duration_seconds = parsed;
            } else {
                config->payload_bytes = parsed;
            }
            continue;
        }
        *error = "unknown argument: " + std::string(arg);
        return false;
    }

    if (config->work_dir.empty()) {
        *error = "--work-dir is required";
        return false;
    }
    if (config->duration_seconds == 0U) {
        *error = "duration must be positive";
        return false;
    }
    if (config->payload_bytes < kMinimumPayloadBytes) {
        *error = "payload must be at least 4096 bytes";
        return false;
    }
    if (config->certification && config->duration_seconds < kCertificationSeconds) {
        *error = "certification mode requires at least 86400 seconds";
        return false;
    }
    return true;
}

std::uint64_t process_id() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

std::uint64_t elapsed_ms(Clock::time_point started) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
}

std::string json_escape(std::string_view text) {
    std::ostringstream stream;
    for (const unsigned char value : text) {
        switch (value) {
        case '"': stream << "\\\""; break;
        case '\\': stream << "\\\\"; break;
        case '\b': stream << "\\b"; break;
        case '\f': stream << "\\f"; break;
        case '\n': stream << "\\n"; break;
        case '\r': stream << "\\r"; break;
        case '\t': stream << "\\t"; break;
        default:
            if (value < 0x20U) {
                stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned int>(value) << std::dec;
            } else {
                stream << static_cast<char>(value);
            }
            break;
        }
    }
    return stream.str();
}

void digest_mix(std::uint64_t value, std::uint64_t* digest) {
    constexpr std::uint64_t kOffset = 1469598103934665603ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    if (*digest == 0U) {
        *digest = kOffset;
    }
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        *digest ^= (value >> shift) & 0xffU;
        *digest *= kPrime;
    }
}

void digest_receipt(BenchmarkSessionMode mode, const BenchmarkQueryReceipt& receipt, SoakState* state) {
    digest_mix(static_cast<std::uint64_t>(mode), &state->rolling_digest);
    digest_mix(receipt.coordinate, &state->rolling_digest);
    digest_mix(receipt.source_bytes_read, &state->rolling_digest);
    digest_mix(receipt.rendered_height_q8, &state->rolling_digest);
    digest_mix(receipt.checkpoint_source_offset, &state->rolling_digest);
    digest_mix(static_cast<std::uint64_t>(receipt.fragment_count), &state->rolling_digest);
    digest_mix(receipt.truncated ? 1U : 0U, &state->rolling_digest);
}

std::string hex64(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

class EventWriter final {
public:
    bool open(const Config& config, std::string* error) {
        if (!config.output) {
            return true;
        }
        std::error_code fs_error;
        if (!config.output->parent_path().empty()) {
            std::filesystem::create_directories(config.output->parent_path(), fs_error);
            if (fs_error) {
                *error = "cannot create soak report directory: " + fs_error.message();
                return false;
            }
        }
        file_.open(*config.output, std::ios::binary | std::ios::trunc);
        if (!file_) {
            *error = "cannot create soak event log";
            return false;
        }
        return true;
    }

    bool write(const std::string& event, std::string* error) {
        std::cout << event << '\n' << std::flush;
        if (!file_.is_open()) {
            return true;
        }
        file_ << event << '\n' << std::flush;
        if (!file_) {
            *error = "cannot write soak event log";
            return false;
        }
        return true;
    }

private:
    std::ofstream file_;
};

bool capture_memory(SoakState* state, std::string* error) {
    ZenithProcessMemorySnapshot snapshot;
    std::string capture_error;
    if (!zevryon::massivedoc::capture_zenith_process_memory_snapshot(&snapshot, &capture_error)) {
        ++state->memory_snapshot_failures;
        *error = "memory snapshot failed: " + capture_error;
        return false;
    }
    ++state->memory_samples;
    state->current_rss_bytes = snapshot.process_rss_bytes;
    state->peak_rss_bytes = std::max(state->peak_rss_bytes, snapshot.process_rss_bytes);
    return true;
}

bool validate_query(
    BenchmarkSessionMode mode,
    const BenchmarkQueryReceipt& receipt,
    std::string* error) {
    if (receipt.milliseconds < 0.0) {
        *error = "query timing became negative";
        return false;
    }
    if (mode == BenchmarkSessionMode::Virtualized) {
        if (receipt.source_bytes_read == 0U || receipt.rendered_height_q8 == 0U) {
            *error = "virtualized query produced an empty read/layout receipt";
            return false;
        }
    } else if (receipt.fragment_count == 0U || receipt.rendered_height_q8 == 0U) {
        *error = "native query produced an empty fragment/layout receipt";
        return false;
    }
    return true;
}

std::string start_event(
    const Config& config,
    const BenchmarkSyntheticStoreReady& store,
    double setup_seconds,
    std::uint64_t checkpoint_ms,
    std::uint64_t memory_sample_ms) {
    std::ostringstream stream;
    stream << std::setprecision(17)
           << "{\"schema\":\"zevryon.m8.soak-event.v1\","
           << "\"authority\":\"m8-continuous-dual-mode-soak-v1\","
           << "\"event\":\"start\","
           << "\"process_id\":" << process_id() << ','
           << "\"mode\":\"" << (config.certification ? "certification" : "smoke") << "\","
           << "\"duration_seconds_requested\":" << config.duration_seconds << ','
           << "\"certification_minimum_seconds\":" << kCertificationSeconds << ','
           << "\"payload_bytes\":" << store.payload_bytes << ','
           << "\"payload_sha256\":\"" << json_escape(store.payload_sha256) << "\","
           << "\"checkpoint_interval_ms\":" << checkpoint_ms << ','
           << "\"memory_sample_interval_ms\":" << memory_sample_ms << ','
           << "\"setup_seconds\":" << setup_seconds
           << '}';
    return stream.str();
}

std::string checkpoint_event(const SoakState& state, std::uint64_t elapsed) {
    std::ostringstream stream;
    stream << std::setprecision(17)
           << "{\"schema\":\"zevryon.m8.soak-event.v1\","
           << "\"authority\":\"m8-continuous-dual-mode-soak-v1\","
           << "\"event\":\"checkpoint\","
           << "\"process_id\":" << process_id() << ','
           << "\"ordinal\":" << state.checkpoints << ','
           << "\"elapsed_ms\":" << elapsed << ','
           << "\"virtualized_queries\":" << state.virtualized_queries << ','
           << "\"native_queries\":" << state.native_queries << ','
           << "\"rolling_digest\":\"" << hex64(state.rolling_digest) << "\","
           << "\"current_rss_bytes\":" << state.current_rss_bytes << ','
           << "\"peak_rss_bytes\":" << state.peak_rss_bytes << ','
           << "\"memory_samples\":" << state.memory_samples << ','
           << "\"max_virtualized_query_ms\":" << state.max_virtualized_query_ms << ','
           << "\"max_native_query_ms\":" << state.max_native_query_ms
           << '}';
    return stream.str();
}

std::string complete_event(
    const Config& config,
    const SoakState& state,
    std::uint64_t elapsed,
    std::uint64_t checkpoint_ms,
    bool passed,
    std::string_view failure_reason) {
    const std::uint64_t target_ms = config.duration_seconds * 1000ULL;
    const std::uint64_t minimum_checkpoints = target_ms / checkpoint_ms;
    const bool duration_met = elapsed >= target_ms;
    const bool checkpoint_coverage = state.checkpoints >= minimum_checkpoints;
    const bool dual_mode_queries = state.virtualized_queries > 0U && state.native_queries > 0U;
    const bool certification_eligible = config.certification && duration_met && checkpoint_coverage &&
        dual_mode_queries && passed;
    const bool gate_passed = passed && duration_met && checkpoint_coverage && dual_mode_queries &&
        (!config.certification || certification_eligible);

    std::ostringstream stream;
    stream << std::setprecision(17)
           << "{\"schema\":\"zevryon.m8.soak-event.v1\","
           << "\"authority\":\"m8-continuous-dual-mode-soak-v1\","
           << "\"event\":\"complete\","
           << "\"process_id\":" << process_id() << ','
           << "\"mode\":\"" << (config.certification ? "certification" : "smoke") << "\","
           << "\"elapsed_ms\":" << elapsed << ','
           << "\"duration_target_met\":" << (duration_met ? "true" : "false") << ','
           << "\"checkpoint_coverage_met\":" << (checkpoint_coverage ? "true" : "false") << ','
           << "\"checkpoint_count\":" << state.checkpoints << ','
           << "\"minimum_checkpoint_count\":" << minimum_checkpoints << ','
           << "\"virtualized_queries\":" << state.virtualized_queries << ','
           << "\"native_queries\":" << state.native_queries << ','
           << "\"rolling_digest\":\"" << hex64(state.rolling_digest) << "\","
           << "\"current_rss_bytes\":" << state.current_rss_bytes << ','
           << "\"peak_rss_bytes\":" << state.peak_rss_bytes << ','
           << "\"memory_samples\":" << state.memory_samples << ','
           << "\"memory_snapshot_failures\":" << state.memory_snapshot_failures << ','
           << "\"query_failures\":" << state.query_failures << ','
           << "\"max_virtualized_query_ms\":" << state.max_virtualized_query_ms << ','
           << "\"max_native_query_ms\":" << state.max_native_query_ms << ','
           << "\"certification_eligible\":" << (certification_eligible ? "true" : "false") << ','
           << "\"failure_reason\":";
    if (failure_reason.empty()) {
        stream << "null";
    } else {
        stream << '"' << json_escape(failure_reason) << '"';
    }
    stream << ",\"gate_passed\":" << (gate_passed ? "true" : "false") << '}';
    return stream.str();
}

bool query_once(
    MassiveDocBenchmarkSession* session,
    DeterministicBenchmarkQueryGenerator* generator,
    BenchmarkSessionMode mode,
    SoakState* state,
    std::string* error) {
    BenchmarkQueryReceipt receipt;
    if (!session->query(generator->next(), &receipt, error)) {
        ++state->query_failures;
        return false;
    }
    if (!validate_query(mode, receipt, error)) {
        ++state->query_failures;
        return false;
    }
    digest_receipt(mode, receipt, state);
    if (mode == BenchmarkSessionMode::Virtualized) {
        ++state->virtualized_queries;
        state->max_virtualized_query_ms = std::max(state->max_virtualized_query_ms, receipt.milliseconds);
    } else {
        ++state->native_queries;
        state->max_native_query_ms = std::max(state->max_native_query_ms, receipt.milliseconds);
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Config config;
    std::string error;
    if (!parse_args(argc, argv, &config, &error)) {
        std::cerr << "M8 soak configuration invalid: " << error << '\n';
        return 1;
    }

    EventWriter writer;
    if (!writer.open(config, &error)) {
        std::cerr << "M8 soak event-log initialization failed: " << error << '\n';
        return 1;
    }

    std::error_code fs_error;
    std::filesystem::remove_all(config.work_dir, fs_error);
    if (fs_error) {
        std::cerr << "M8 soak work-dir cleanup failed: " << fs_error.message() << '\n';
        return 1;
    }
    std::filesystem::create_directories(config.work_dir, fs_error);
    if (fs_error) {
        std::cerr << "M8 soak work-dir creation failed: " << fs_error.message() << '\n';
        return 1;
    }

    const auto setup_started = Clock::now();
    const auto store_root = config.work_dir / "store";
    BenchmarkSyntheticStoreReady store_ready;
    if (!zevryon::massivedoc::build_m7_synthetic_benchmark_store(
            store_root,
            config.payload_bytes,
            &store_ready,
            &error)) {
        std::cerr << "M8 soak synthetic store build failed: " << error << '\n';
        return 2;
    }

    BenchmarkSessionConfig session_config;
    session_config.store_root = store_root;
    session_config.record_index = store_ready.record_index;
    session_config.payload_bytes = store_ready.payload_bytes;
    session_config.virtual_slice_bytes = 128U * 1024U;
    session_config.viewport_width_px = 800U;
    session_config.viewport_height_px = 720U;
    session_config.max_fragments = 512U;
    session_config.checkpoint_stride_bytes = 64U * 1024U;

    MassiveDocBenchmarkSession virtualized;
    MassiveDocBenchmarkSession native;
    BenchmarkSessionReady virtual_ready;
    BenchmarkSessionReady native_ready;
    if (!virtualized.open(BenchmarkSessionMode::Virtualized, session_config, &virtual_ready, &error)) {
        std::cerr << "M8 soak virtualized open failed: " << error << '\n';
        return 2;
    }
    if (!native.open(BenchmarkSessionMode::NativeDom, session_config, &native_ready, &error)) {
        std::cerr << "M8 soak native open failed: " << error << '\n';
        return 2;
    }

    DeterministicBenchmarkQueryGenerator virtual_generator(
        BenchmarkSessionMode::Virtualized,
        store_ready.payload_bytes,
        session_config.virtual_slice_bytes);
    DeterministicBenchmarkQueryGenerator native_generator(
        BenchmarkSessionMode::NativeDom,
        store_ready.payload_bytes,
        session_config.virtual_slice_bytes);

    SoakState state;
    for (std::size_t index = 0U; index < kWarmupQueriesPerMode; ++index) {
        if (!query_once(&virtualized, &virtual_generator, BenchmarkSessionMode::Virtualized, &state, &error) ||
            !query_once(&native, &native_generator, BenchmarkSessionMode::NativeDom, &state, &error)) {
            std::cerr << "M8 soak warmup failed: " << error << '\n';
            return 2;
        }
    }
    state = SoakState{};

    const double setup_seconds = std::chrono::duration<double>(Clock::now() - setup_started).count();
    const std::uint64_t checkpoint_ms = config.certification ? kCertificationCheckpointMs : kSmokeCheckpointMs;
    const std::uint64_t memory_sample_ms = config.certification ? kCertificationMemorySampleMs : kSmokeMemorySampleMs;
    if (!writer.write(start_event(config, store_ready, setup_seconds, checkpoint_ms, memory_sample_ms), &error)) {
        std::cerr << "M8 soak start receipt failure: " << error << '\n';
        return 1;
    }

    const auto started = Clock::now();
    std::uint64_t next_checkpoint_ms = checkpoint_ms;
    std::uint64_t next_memory_sample_ms = 0U;
    bool passed = true;
    std::string failure_reason;

    while (true) {
        if (!query_once(&virtualized, &virtual_generator, BenchmarkSessionMode::Virtualized, &state, &error) ||
            !query_once(&native, &native_generator, BenchmarkSessionMode::NativeDom, &state, &error)) {
            passed = false;
            failure_reason = error;
            break;
        }

        const std::uint64_t elapsed = elapsed_ms(started);
        if (elapsed >= next_memory_sample_ms) {
            if (!capture_memory(&state, &error)) {
                passed = false;
                failure_reason = error;
                break;
            }
            next_memory_sample_ms = elapsed + memory_sample_ms;
        }

        while (elapsed >= next_checkpoint_ms) {
            ++state.checkpoints;
            if (!writer.write(checkpoint_event(state, elapsed), &error)) {
                passed = false;
                failure_reason = error;
                break;
            }
            next_checkpoint_ms += checkpoint_ms;
        }
        if (!passed) {
            break;
        }

        if (elapsed >= config.duration_seconds * 1000ULL) {
            break;
        }
    }

    const std::uint64_t final_elapsed = elapsed_ms(started);
    if (state.memory_samples == 0U && passed) {
        if (!capture_memory(&state, &error)) {
            passed = false;
            failure_reason = error;
        }
    }

    const auto terminal = complete_event(
        config,
        state,
        final_elapsed,
        checkpoint_ms,
        passed,
        failure_reason);
    std::string terminal_error;
    if (!writer.write(terminal, &terminal_error)) {
        std::cerr << "M8 soak terminal receipt failure: " << terminal_error << '\n';
        return 1;
    }

    if (!passed) {
        std::cerr << "M8 soak failure: " << failure_reason << '\n';
        return 2;
    }

    const std::uint64_t minimum_checkpoints = (config.duration_seconds * 1000ULL) / checkpoint_ms;
    if (final_elapsed < config.duration_seconds * 1000ULL ||
        state.checkpoints < minimum_checkpoints ||
        state.virtualized_queries == 0U || state.native_queries == 0U) {
        std::cerr << "M8 soak terminal coverage gate failed\n";
        return 2;
    }
    return 0;
}
