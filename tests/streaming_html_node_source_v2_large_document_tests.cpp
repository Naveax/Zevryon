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

constexpr std::size_t kPayloadChunkBytes = 64U * 1024U;
constexpr std::size_t kPayloadRecordCount = 128U;
constexpr std::uint64_t kPayloadChunkBytesU64 =
    static_cast<std::uint64_t>(kPayloadChunkBytes);
constexpr std::uint64_t kPayloadRecordCountU64 =
    static_cast<std::uint64_t>(kPayloadRecordCount);
constexpr std::uint64_t kPayloadBytes =
    kPayloadChunkBytesU64 * kPayloadRecordCountU64;
constexpr std::size_t kParserWindowBytes = 4096U;
constexpr std::size_t kParserWorkingSetLimitBytes = 256U * 1024U;
constexpr std::string_view kStartTag = "<style>";
constexpr std::string_view kEndTag = "</style>";
constexpr std::uint64_t kStartTagBytes =
    static_cast<std::uint64_t>(kStartTag.size());
constexpr std::uint64_t kEndTagBytes =
    static_cast<std::uint64_t>(kEndTag.size());

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: Z7 bounded large-document parser smoke: "
                  << message << '\n';
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

std::span<const std::byte> bytes(std::string_view text) {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size());
}

bool build_large_store(const std::filesystem::path& store_root, std::string* error) {
    StoreWriter writer(store_root);
    std::uint64_t total = 0U;
    std::uint64_t largest = 0U;

    for (std::size_t index = 0U; index < kPayloadRecordCount; ++index) {
        std::string record;
        record.reserve(
            kPayloadChunkBytes + kStartTag.size() + kEndTag.size());
        if (index == 0U) {
            record.append(kStartTag);
        }
        const unsigned fill_offset = static_cast<unsigned>(index % 26U);
        const char fill = static_cast<char>(
            static_cast<unsigned>('a') + fill_offset);
        record.append(kPayloadChunkBytes, fill);
        if (index + 1U == kPayloadRecordCount) {
            record.append(kEndTag);
        }

        if (!writer.append(
                9701U + static_cast<std::uint64_t>(index),
                bytes(record),
                error)) {
            return false;
        }
        const auto record_bytes = static_cast<std::uint64_t>(record.size());
        total += record_bytes;
        largest = std::max(largest, record_bytes);
    }

    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = total;
    metadata.logical_records = kPayloadRecordCountU64;
    metadata.logical_nodes = 3U;
    metadata.largest_record_bytes = largest;
    return writer.finalize(metadata, nullptr, error);
}

bool inspect_three_nodes(
    const std::filesystem::path& source_path,
    LogicalNodeSourceNode* document,
    LogicalNodeSourceNode* element,
    LogicalNodeSourceNode* text,
    std::string* error) {
    LogicalNodeSourceV2Reader reader(source_path);
    LogicalNodeSourceNode extra;
    bool has_node = false;
    return reader.open(error) &&
        reader.next(document, &has_node, error) && has_node &&
        reader.next(element, &has_node, error) && has_node &&
        reader.next(text, &has_node, error) && has_node &&
        reader.next(&extra, &has_node, error) && !has_node;
}

bool test_large_rawtext_stays_bounded() {
    const std::filesystem::path root = unique_root("html-v2-large-document");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    std::string error;
    if (!require(build_large_store(store_root, &error), error)) {
        return false;
    }

    StreamingHtmlNodeSourceConfig config;
    config.input_window_bytes = kParserWindowBytes;
    config.working_set_limit_bytes = kParserWorkingSetLimitBytes;
    StreamingHtmlNodeSourceV2Stats stats;
    if (!require(produce_streaming_html_node_source_v2(
                     store_root, source_path, config, &stats, &error),
                 error) ||
        !require(stats.nodes_emitted == 3U, "document + style + text node count") ||
        !require(stats.element_nodes_emitted == 1U, "one style element emitted") ||
        !require(stats.text_nodes_emitted == 1U, "one RAWTEXT node emitted") ||
        !require(stats.cross_record_text_spans == 1U,
                 "large RAWTEXT remains one cross-record logical text span") ||
        !require(stats.working_set_hard_limit_bytes ==
                     static_cast<std::uint64_t>(kParserWorkingSetLimitBytes),
                 "parser reports configured 256 KiB hard limit") ||
        !require(stats.working_set_peak_bytes <=
                     static_cast<std::uint64_t>(kParserWorkingSetLimitBytes),
                 "parser working-set peak remains below hard limit") ||
        !require(stats.working_set_peak_bytes < kPayloadBytes / 16U,
                 "parser working set is far smaller than 8 MiB payload") ||
        !require(stats.working_set_current_bytes == 0U,
                 "parser releases all charged working set") ||
        !require(stats.working_set_accounting_errors == 0U,
                 "parser working-set accounting remains clean")) {
        return false;
    }

    const std::uint64_t expected_source_bytes =
        kPayloadBytes + kStartTagBytes + kEndTagBytes;
    if (!require(stats.source_records == kPayloadRecordCountU64,
                 "parser streams every physical source record") ||
        !require(stats.source_bytes == expected_source_bytes,
                 "parser accounts the exact large HTML byte count")) {
        return false;
    }

    LogicalNodeSourceNode document;
    LogicalNodeSourceNode style;
    LogicalNodeSourceNode text;
    if (!require(inspect_three_nodes(
                     source_path, &document, &style, &text, &error),
                 error) ||
        !require(document.tag == "#document", "document semantic survives") ||
        !require(style.tag == "style" && style.parent_ordinal == 0U,
                 "style semantic and parent survive") ||
        !require(style.source_record_index == 0U &&
                     style.source_byte_offset == 0U &&
                     style.source_byte_length == kStartTagBytes,
                 "style start-tag source identity is exact") ||
        !require(text.tag == "#text" && text.parent_ordinal == 1U,
                 "large RAWTEXT node keeps style parent") ||
        !require(text.source_record_index == 0U &&
                     text.source_byte_offset == kStartTagBytes &&
                     text.source_byte_length == kPayloadBytes,
                 "8 MiB RAWTEXT source span is exact across all records")) {
        return false;
    }

    LogicalNodeSourceV2ValidationStats validation;
    return require(validate_logical_node_source_v2_against_store(
                       source_path, store_root, &validation, &error),
                   error) &&
        require(validation.nodes_validated == 3U,
                "authoritative validator accepts large parser output") &&
        require(validation.source_span_bytes_streamed ==
                    kStartTagBytes + kPayloadBytes,
                "validator streams exact node-backed bytes without end-tag inflation");
}

} // namespace

int main() {
    return test_large_rawtext_stays_bounded() ? 0 : 1;
}
