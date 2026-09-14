#include "logical_node_source_v2.hpp"
#include "massivedoc_store.hpp"
#include "streaming_html_node_source_v2.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

using zevryon::massivedoc::CorpusMetadata;
using zevryon::massivedoc::LogicalNodeSourceNode;
using zevryon::massivedoc::LogicalNodeSourceV2Reader;
using zevryon::massivedoc::LogicalNodeSourceV2ValidationStats;
using zevryon::massivedoc::StoreWriter;
using zevryon::massivedoc::StreamingHtmlNodeSourceConfig;
using zevryon::massivedoc::StreamingHtmlNodeSourceV2Stats;
using zevryon::massivedoc::produce_streaming_html_node_source_v2;
using zevryon::massivedoc::validate_logical_node_source_v2_against_store;

constexpr std::size_t kRecordPayloadBytes = 64U * 1024U;
constexpr std::size_t kLargeRecordCount = 1024U;
constexpr std::uint64_t kLargePayloadBytes =
    static_cast<std::uint64_t>(kRecordPayloadBytes) * kLargeRecordCount;
constexpr std::size_t kMalformedRecordCount = 256U;
constexpr std::size_t kParserWindowBytes = 4096U;
constexpr std::size_t kWorkingSetLimitBytes = 256U * 1024U;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: Z7 bounded large-document certification: "
                  << message << '\n';
        return false;
    }
    return true;
}

std::filesystem::path unique_root() {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (std::string("zevryon-z7-large-cert-") + std::to_string(tick));
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

std::span<const std::byte> bytes(std::string_view text) {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size());
}

bool build_large_store(
    const std::filesystem::path& root,
    std::string_view start,
    std::string_view finish,
    std::uint64_t logical_nodes,
    std::size_t record_count,
    std::string* error) {
    StoreWriter writer(root);
    std::uint64_t total = 0U;
    std::uint64_t largest = 0U;
    for (std::size_t index = 0U; index < record_count; ++index) {
        std::string record;
        record.reserve(kRecordPayloadBytes + start.size() + finish.size());
        if (index == 0U) {
            record.append(start);
        }
        const char fill = static_cast<char>('a' + static_cast<int>(index % 26U));
        record.append(kRecordPayloadBytes, fill);
        if (index + 1U == record_count) {
            record.append(finish);
        }
        if (!writer.append(
                12'001U + static_cast<std::uint64_t>(index),
                bytes(record),
                error)) {
            return false;
        }
        const auto size = static_cast<std::uint64_t>(record.size());
        total += size;
        largest = std::max(largest, size);
    }
    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = total;
    metadata.logical_records = static_cast<std::uint64_t>(record_count);
    metadata.logical_nodes = logical_nodes;
    metadata.largest_record_bytes = largest;
    return writer.finalize(metadata, nullptr, error);
}

bool inspect_three_nodes(
    const std::filesystem::path& source,
    LogicalNodeSourceNode* document,
    LogicalNodeSourceNode* element,
    LogicalNodeSourceNode* text,
    std::string* error) {
    LogicalNodeSourceV2Reader reader(source);
    LogicalNodeSourceNode extra;
    bool has = false;
    return reader.open(error) &&
        reader.next(document, &has, error) && has &&
        reader.next(element, &has, error) && has &&
        reader.next(text, &has, error) && has &&
        reader.next(&extra, &has, error) && !has;
}

bool certify_success_case(
    const std::filesystem::path& root,
    std::string_view label,
    std::string_view start,
    std::string_view finish,
    std::string_view expected_tag) {
    const auto store = root / (std::string(label) + "-store");
    const auto source = root / (std::string(label) + ".zvnsrc");
    std::string error;
    if (!require(build_large_store(
                     store, start, finish, 3U, kLargeRecordCount, &error),
                 error)) {
        return false;
    }

    StreamingHtmlNodeSourceConfig config;
    config.input_window_bytes = kParserWindowBytes;
    config.working_set_limit_bytes = kWorkingSetLimitBytes;
    StreamingHtmlNodeSourceV2Stats stats;
    const auto started = std::chrono::steady_clock::now();
    if (!require(produce_streaming_html_node_source_v2(
                     store, source, config, &stats, &error),
                 error)) {
        return false;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    if (!require(stats.source_records == kLargeRecordCount,
                 "large case source-record count drifted") ||
        !require(stats.nodes_emitted == 3U,
                 "large case node count drifted") ||
        !require(stats.element_nodes_emitted == 1U,
                 "large case element count drifted") ||
        !require(stats.text_nodes_emitted == 1U,
                 "large case text count drifted") ||
        !require(stats.cross_record_text_spans == 1U,
                 "large text was not preserved as one cross-record span") ||
        !require(stats.working_set_hard_limit_bytes == kWorkingSetLimitBytes,
                 "large case hard working-set limit drifted") ||
        !require(stats.working_set_peak_bytes <= kWorkingSetLimitBytes,
                 "large case exceeded parser working-set hard limit") ||
        !require(stats.working_set_peak_bytes < kLargePayloadBytes / 64U,
                 "large parser working set scales with payload") ||
        !require(stats.working_set_current_bytes == 0U,
                 "large parser leaked charged working set") ||
        !require(stats.working_set_accounting_errors == 0U,
                 "large parser reported accounting error")) {
        return false;
    }

    LogicalNodeSourceNode document;
    LogicalNodeSourceNode element;
    LogicalNodeSourceNode text;
    if (!require(inspect_three_nodes(
                     source, &document, &element, &text, &error),
                 error) ||
        !require(document.tag == "#document", "document node missing") ||
        !require(element.tag == expected_tag && element.parent_ordinal == 0U,
                 "large element semantic drifted") ||
        !require(text.tag == "#text" && text.parent_ordinal == 1U,
                 "large text semantic drifted") ||
        !require(text.source_record_index == 0U,
                 "large text source record drifted") ||
        !require(text.source_byte_offset == start.size(),
                 "large text source offset drifted") ||
        !require(text.source_byte_length == kLargePayloadBytes,
                 "large text source length drifted")) {
        return false;
    }

    LogicalNodeSourceV2ValidationStats validation;
    if (!require(validate_logical_node_source_v2_against_store(
                     source, store, &validation, &error),
                 error) ||
        !require(validation.nodes_validated == 3U,
                 "large source validator node count drifted") ||
        !require(validation.source_span_bytes_streamed ==
                     static_cast<std::uint64_t>(start.size()) + kLargePayloadBytes,
                 "large source validator byte count drifted")) {
        return false;
    }

    std::cout << "CERT\t" << label
              << "\tpayload_bytes=" << kLargePayloadBytes
              << "\tworking_set_peak=" << stats.working_set_peak_bytes
              << "\telapsed_ms=" << elapsed.count() << '\n';
    return true;
}

bool certify_bounded_rejection(const std::filesystem::path& root) {
    const auto store = root / "malformed-store";
    const auto source = root / "malformed.zvnsrc";
    std::string error;
    if (!require(build_large_store(
                     store, "<div ", "", 1U, kMalformedRecordCount, &error),
                 error)) {
        return false;
    }

    StreamingHtmlNodeSourceConfig config;
    config.input_window_bytes = kParserWindowBytes;
    config.maximum_token_bytes = 1024U * 1024U;
    config.working_set_limit_bytes = 2U * 1024U * 1024U;
    StreamingHtmlNodeSourceV2Stats stats;
    if (!require(!produce_streaming_html_node_source_v2(
                     store, source, config, &stats, &error),
                 "oversized malformed token unexpectedly succeeded") ||
        !require(error.find("HTML markup token exceeds bounded byte limit") != std::string::npos,
                 "oversized malformed token returned wrong failure class") ||
        !require(stats.working_set_current_bytes == 0U,
                 "malformed rejection leaked charged working set") ||
        !require(stats.working_set_accounting_errors == 0U,
                 "malformed rejection reported accounting error") ||
        !require(!std::filesystem::exists(source),
                 "malformed rejection published final source") ||
        !require(!std::filesystem::exists(
                     std::filesystem::path(source.string() + ".building")),
                 "malformed rejection left staging source")) {
        return false;
    }
    std::cout << "CERT\tmalformed-oversized-token\trejected=1\n";
    return true;
}

} // namespace

int main() {
    const auto root = unique_root();
    RootCleanup cleanup(root);
    std::error_code filesystem_error;
    std::filesystem::create_directories(root, filesystem_error);
    if (!require(!filesystem_error, "cannot create certification root")) {
        return 1;
    }

    if (!certify_success_case(root, "data", "<div>", "</div>", "div") ||
        !certify_success_case(root, "rawtext", "<style>", "</style>", "style") ||
        !certify_bounded_rejection(root)) {
        return 1;
    }

    std::cout << "Z7 bounded large-document certification PASS"
              << " payload_bytes_per_success=" << kLargePayloadBytes
              << " success_cases=2 rejection_cases=1\n";
    return 0;
}
