#include "massivedoc_benchmark_session.hpp"

#include "compact_document.hpp"
#include "layout_checkpoint.hpp"
#include "massivedoc_store.hpp"
#include "unicode_stream.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory_resource>
#include <span>
#include <utility>
#include <vector>

namespace zevryon::massivedoc {
namespace {

constexpr std::uint64_t kLcgDivisor = 0xffffffffULL;
constexpr std::uint64_t kNativeCoordinateMaximum = 999999ULL;
constexpr std::uint64_t kNativeCoordinateDivisor = 1000000ULL;
constexpr std::uint64_t kQ8 = 256ULL;
constexpr std::uint64_t kAverageAdvanceQ8 = 8ULL * kQ8;
constexpr std::uint64_t kLineHeightQ8 = 18ULL * kQ8;
constexpr std::uint64_t kHorizontalPaddingQ8 = 12ULL * kQ8;
constexpr std::uint64_t kVerticalPaddingQ8 = 12ULL * kQ8;

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

std::uint64_t saturating_multiply(std::uint64_t left, std::uint64_t right) noexcept {
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left * right;
}

std::uint64_t scale_ratio(
    std::uint64_t numerator,
    std::uint64_t value,
    std::uint64_t denominator) noexcept {
    if (denominator == 0U) {
        return 0U;
    }
    const std::uint64_t quotient = value / denominator;
    const std::uint64_t remainder = value % denominator;
    return saturating_add(
        saturating_multiply(quotient, numerator),
        saturating_multiply(remainder, numerator) / denominator);
}

bool valid_config(const BenchmarkSessionConfig& config, std::string* error) {
    if (error == nullptr) {
        return false;
    }
    if (config.store_root.empty()) {
        *error = "benchmark session store root cannot be empty";
        return false;
    }
    if (config.payload_bytes == 0U) {
        *error = "benchmark session payload bytes must be positive";
        return false;
    }
    if (config.virtual_slice_bytes == 0U ||
        config.virtual_slice_bytes > kMaximumMaterializedRecordSliceBytes) {
        *error = "benchmark session virtual slice is outside the bounded materialization limit";
        return false;
    }
    if (config.viewport_width_px == 0U || config.viewport_height_px == 0U ||
        config.viewport_width_px > std::numeric_limits<std::uint32_t>::max() / 256U ||
        config.viewport_height_px > std::numeric_limits<std::uint32_t>::max() / 256U) {
        *error = "benchmark session viewport is outside the Q8 range";
        return false;
    }
    if (config.max_fragments == 0U) {
        *error = "benchmark session max fragments must be positive";
        return false;
    }
    if (config.checkpoint_stride_bytes == 0U) {
        *error = "benchmark session checkpoint stride must be positive";
        return false;
    }
    error->clear();
    return true;
}

bool virtual_layout_receipt(
    std::span<const std::byte> bytes,
    std::uint32_t viewport_width_px,
    std::uint64_t* rendered_height_q8,
    std::string* error) {
    if (rendered_height_q8 == nullptr || error == nullptr) {
        return false;
    }

    zevryon::text::Utf8StreamDecoder decoder(zevryon::text::Utf8ErrorPolicy::Replace);
    std::pmr::vector<zevryon::text::DecodedCodePoint> decoded;
    zevryon::text::Utf8DecodeError decode_error;
    if (!decoder.feed(bytes, 0U, &decoded, &decode_error) ||
        !decoder.finish(&decoded, &decode_error)) {
        *error = "benchmark session UTF-8 decode failed: " + decode_error.message;
        return false;
    }

    const std::uint64_t width_q8 =
        static_cast<std::uint64_t>(viewport_width_px) * kQ8;
    const std::uint64_t horizontal_q8 = kHorizontalPaddingQ8 * 2U;
    const std::uint64_t content_width_q8 =
        width_q8 > horizontal_q8 ? width_q8 - horizontal_q8 : kAverageAdvanceQ8;
    const std::uint64_t columns_per_line =
        std::max<std::uint64_t>(1U, content_width_q8 / kAverageAdvanceQ8);

    std::uint64_t completed_lines = 0U;
    std::uint64_t columns = 0U;
    for (const auto& codepoint : decoded) {
        if (codepoint.value == 0x0aU) {
            completed_lines = saturating_add(completed_lines, 1U);
            columns = 0U;
            continue;
        }
        columns = saturating_add(columns, codepoint.value == 0x09U ? 4U : 1U);
        while (columns >= columns_per_line) {
            completed_lines = saturating_add(completed_lines, 1U);
            columns -= columns_per_line;
        }
    }

    std::uint64_t total_lines = completed_lines;
    if (columns != 0U || decoded.empty()) {
        total_lines = saturating_add(total_lines, 1U);
    }
    *rendered_height_q8 = saturating_add(
        kVerticalPaddingQ8,
        saturating_multiply(total_lines, kLineHeightQ8));
    error->clear();
    return true;
}

} // namespace

struct MassiveDocBenchmarkSession::Impl {
    BenchmarkSessionMode mode{BenchmarkSessionMode::Virtualized};
    BenchmarkSessionConfig config{};
    std::unique_ptr<StoreReader> store_reader;
    std::unique_ptr<LayoutCheckpointIndex> checkpoint_index;
    MaterializedRecord native_record{};
    std::uint64_t native_total_height_q8{0U};
    std::uint64_t native_checkpoint_bytes{0U};
    bool opened{false};
};

DeterministicBenchmarkQueryGenerator::DeterministicBenchmarkQueryGenerator(
    BenchmarkSessionMode mode,
    std::uint64_t payload_bytes,
    std::size_t virtual_slice_bytes) {
    if (mode == BenchmarkSessionMode::NativeDom) {
        maximum_ = kNativeCoordinateMaximum;
        return;
    }
    const std::uint64_t slice_bytes = static_cast<std::uint64_t>(virtual_slice_bytes);
    maximum_ = payload_bytes > slice_bytes ? payload_bytes - slice_bytes : 0U;
}

std::uint64_t DeterministicBenchmarkQueryGenerator::next() noexcept {
    state_ = static_cast<std::uint32_t>(
        state_ * static_cast<std::uint32_t>(1664525U) +
        static_cast<std::uint32_t>(1013904223U));
    if (maximum_ == 0U) {
        return 0U;
    }
    return scale_ratio(static_cast<std::uint64_t>(state_), maximum_, kLcgDivisor);
}

MassiveDocBenchmarkSession::MassiveDocBenchmarkSession()
    : impl_(std::make_unique<Impl>()) {}

MassiveDocBenchmarkSession::~MassiveDocBenchmarkSession() = default;

bool MassiveDocBenchmarkSession::open(
    BenchmarkSessionMode mode,
    BenchmarkSessionConfig config,
    BenchmarkSessionReady* ready,
    std::string* error) {
    if (ready == nullptr || error == nullptr) {
        return false;
    }
    if (impl_->opened) {
        *error = "benchmark session is already open";
        return false;
    }
    if (!valid_config(config, error)) {
        return false;
    }

    impl_->mode = mode;
    impl_->config = std::move(config);
    impl_->store_reader = std::make_unique<StoreReader>(impl_->config.store_root);
    if (!impl_->store_reader->open(error)) {
        impl_->store_reader.reset();
        return false;
    }

    if (mode == BenchmarkSessionMode::NativeDom) {
        LayoutCheckpointConfig checkpoint_config;
        checkpoint_config.width_q8 = impl_->config.viewport_width_px * 256U;
        checkpoint_config.target_stride_bytes = impl_->config.checkpoint_stride_bytes;
        LayoutCheckpointStats checkpoint_stats;
        if (!build_layout_checkpoint(
                impl_->config.store_root,
                impl_->config.record_index,
                checkpoint_config,
                &checkpoint_stats,
                error)) {
            return false;
        }
        if (checkpoint_stats.source_bytes != impl_->config.payload_bytes) {
            *error = "native benchmark checkpoint source length drifted from declared payload";
            return false;
        }
        if (checkpoint_stats.height_saturated) {
            *error = "native benchmark checkpoint height saturated the Q8 authority";
            return false;
        }
        impl_->checkpoint_index = std::make_unique<LayoutCheckpointIndex>();
        if (!impl_->checkpoint_index->open(
                impl_->config.store_root,
                impl_->config.record_index,
                checkpoint_config,
                error)) {
            impl_->checkpoint_index.reset();
            return false;
        }
        impl_->native_record.record_index = checkpoint_stats.record_index;
        impl_->native_record.source_record_index = checkpoint_stats.record_index;
        impl_->native_record.logical_id = checkpoint_stats.logical_id;
        impl_->native_record.height_q8 = checkpoint_stats.measured_height_q8;
        impl_->native_record.source_bytes = checkpoint_stats.source_bytes;
        impl_->native_total_height_q8 = checkpoint_stats.measured_height_q8;
        impl_->native_checkpoint_bytes = checkpoint_stats.physical_bytes;
    }

    impl_->opened = true;
    ready->mode = mode;
    ready->payload_bytes = impl_->config.payload_bytes;
    ready->native_total_height_q8 = impl_->native_total_height_q8;
    ready->native_checkpoint_bytes = impl_->native_checkpoint_bytes;
    error->clear();
    return true;
}

bool MassiveDocBenchmarkSession::query(
    std::uint64_t coordinate,
    BenchmarkQueryReceipt* receipt,
    std::string* error) {
    if (receipt == nullptr || error == nullptr) {
        return false;
    }
    if (!impl_->opened || impl_->store_reader == nullptr) {
        *error = "benchmark session is not open";
        return false;
    }

    const auto started = std::chrono::steady_clock::now();
    BenchmarkQueryReceipt output;
    output.coordinate = coordinate;

    if (impl_->mode == BenchmarkSessionMode::Virtualized) {
        const std::uint64_t slice_bytes =
            static_cast<std::uint64_t>(impl_->config.virtual_slice_bytes);
        const std::uint64_t maximum =
            impl_->config.payload_bytes > slice_bytes
                ? impl_->config.payload_bytes - slice_bytes
                : 0U;
        if (coordinate > maximum) {
            *error = "virtualized benchmark coordinate exceeds the declared payload range";
            return false;
        }
        const std::uint64_t remaining = impl_->config.payload_bytes - coordinate;
        const std::size_t requested =
            remaining < slice_bytes
                ? static_cast<std::size_t>(remaining)
                : impl_->config.virtual_slice_bytes;
        std::vector<std::byte> bytes;
        if (!impl_->store_reader->read_record_slice(
                impl_->config.record_index,
                coordinate,
                requested,
                &bytes,
                error)) {
            return false;
        }
        if (bytes.size() != requested) {
            *error = "virtualized benchmark slice length drifted from the declared payload";
            return false;
        }
        if (!virtual_layout_receipt(
                bytes,
                impl_->config.viewport_width_px,
                &output.rendered_height_q8,
                error)) {
            return false;
        }
        output.source_bytes_read = static_cast<std::uint64_t>(bytes.size());
    } else {
        if (coordinate > kNativeCoordinateMaximum) {
            *error = "native benchmark coordinate must be in 0..999999";
            return false;
        }
        if (impl_->checkpoint_index == nullptr) {
            *error = "native benchmark checkpoint is unavailable";
            return false;
        }
        const std::uint64_t viewport_height_q8 =
            static_cast<std::uint64_t>(impl_->config.viewport_height_px) * kQ8;
        const std::uint64_t maximum_scroll_q8 =
            impl_->native_total_height_q8 > viewport_height_q8
                ? impl_->native_total_height_q8 - viewport_height_q8
                : 0U;
        const std::uint64_t visible_start_q8 = scale_ratio(
            coordinate,
            maximum_scroll_q8,
            kNativeCoordinateDivisor);
        const std::uint64_t visible_end_q8 =
            saturating_add(visible_start_q8, viewport_height_q8);
        std::vector<LayoutFragment> fragments;
        std::uint64_t source_bytes_read = 0U;
        std::uint64_t checkpoint_source_offset = 0U;
        bool truncated = false;
        if (!scan_layout_window_from_checkpoint(
                *impl_->store_reader,
                impl_->native_record,
                *impl_->checkpoint_index,
                visible_start_q8,
                visible_end_q8,
                impl_->config.max_fragments,
                &fragments,
                &source_bytes_read,
                &checkpoint_source_offset,
                &truncated,
                error)) {
            return false;
        }
        if (fragments.empty()) {
            *error = "native benchmark query returned no layout fragments";
            return false;
        }
        output.source_bytes_read = source_bytes_read;
        output.rendered_height_q8 = impl_->native_total_height_q8;
        output.checkpoint_source_offset = checkpoint_source_offset;
        output.fragment_count = fragments.size();
        output.truncated = truncated;
    }

    output.milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    *receipt = output;
    error->clear();
    return true;
}

bool MassiveDocBenchmarkSession::is_open() const noexcept {
    return impl_->opened;
}

BenchmarkSessionMode MassiveDocBenchmarkSession::mode() const noexcept {
    return impl_->mode;
}

const char* benchmark_session_mode_name(BenchmarkSessionMode mode) noexcept {
    switch (mode) {
        case BenchmarkSessionMode::Virtualized:
            return "virtualized";
        case BenchmarkSessionMode::NativeDom:
            return "native-dom";
    }
    return "invalid";
}

} // namespace zevryon::massivedoc
