#include "logical_node_source_v2.hpp"
#include "massivedoc_store.hpp"
#include "streaming_html_node_source_v2.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

constexpr std::string_view kStartTag = "<div data-x='1'>";
constexpr std::string_view kText = "hello world";

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: streaming HTML node source v2: " << message << '\n';
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

bool build_cross_record_store(
    const std::filesystem::path& root,
    std::string* error) {
    constexpr std::array<std::string_view, 3> records{{
        "<div da",
        "ta-x='1'>hel",
        "lo world</div>",
    }};
    StoreWriter writer(root);
    std::uint64_t total = 0U;
    std::uint64_t largest = 0U;
    for (std::size_t index = 0U; index < records.size(); ++index) {
        if (!writer.append(
                8101U + static_cast<std::uint64_t>(index),
                bytes(records[index]),
                error)) {
            return false;
        }
        total += static_cast<std::uint64_t>(records[index].size());
        largest = std::max<std::uint64_t>(largest, records[index].size());
    }
    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = total;
    metadata.logical_records = records.size();
    metadata.logical_nodes = 3U; // #document + div + #text
    metadata.largest_record_bytes = largest;
    return writer.finalize(metadata, nullptr, error);
}

bool build_special_text_store(
    const std::filesystem::path& root,
    std::string* error) {
    constexpr std::string_view html = "<script>x</script>";
    StoreWriter writer(root);
    if (!writer.append(8201U, bytes(html), error)) {
        return false;
    }
    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = html.size();
    metadata.logical_records = 1U;
    metadata.logical_nodes = 3U;
    metadata.largest_record_bytes = html.size();
    return writer.finalize(metadata, nullptr, error);
}

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return {};
    }
    const std::streamoff end = stream.tellg();
    if (end < 0) {
        return {};
    }
    std::vector<std::byte> output(static_cast<std::size_t>(end));
    stream.seekg(0, std::ios::beg);
    if (!output.empty()) {
        stream.read(
            reinterpret_cast<char*>(output.data()),
            static_cast<std::streamsize>(output.size()));
    }
    return stream ? output : std::vector<std::byte>{};
}

bool inspect_three_nodes(
    const std::filesystem::path& source,
    LogicalNodeSourceNode* document,
    LogicalNodeSourceNode* element,
    LogicalNodeSourceNode* text,
    std::string* error) {
    LogicalNodeSourceV2Reader reader(source);
    bool has_node = false;
    LogicalNodeSourceNode extra;
    return reader.open(error) &&
        reader.next(document, &has_node, error) && has_node &&
        reader.next(element, &has_node, error) && has_node &&
        reader.next(text, &has_node, error) && has_node &&
        reader.next(&extra, &has_node, error) && !has_node;
}

bool test_cross_record_markup_and_text_round_trip() {
    const std::filesystem::path root = unique_root("html-v2-cross-record");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    std::string error;
    if (!require(build_cross_record_store(store_root, &error), error)) {
        return false;
    }

    StreamingHtmlNodeSourceConfig config;
    config.input_window_bytes = 2U;
    StreamingHtmlNodeSourceV2Stats stats;
    if (!require(produce_streaming_html_node_source_v2(
            store_root, source_path, config, &stats, &error), error) ||
        !require(stats.nodes_emitted == 3U, "emits document, element and text") ||
        !require(stats.element_nodes_emitted == 1U, "element count") ||
        !require(stats.text_nodes_emitted == 1U, "text count") ||
        !require(stats.attributes_emitted == 1U, "attribute count") ||
        !require(stats.cross_record_markup_spans == 1U, "markup span crosses records") ||
        !require(stats.cross_record_text_spans == 1U, "text span crosses records") ||
        !require(stats.working_set_current_bytes == 0U, "working set fully released") ||
        !require(stats.working_set_accounting_errors == 0U, "working set accounting clean") ||
        !require(stats.working_set_peak_bytes <= config.working_set_limit_bytes,
                 "working set respects hard cap")) {
        return false;
    }

    LogicalNodeSourceNode document;
    LogicalNodeSourceNode element;
    LogicalNodeSourceNode text;
    if (!require(inspect_three_nodes(
            source_path, &document, &element, &text, &error), error) ||
        !require(document.tag == "#document", "document semantic") ||
        !require(element.tag == "div", "element tag") ||
        !require(element.parent_ordinal == 0U, "element parent") ||
        !require(element.source_record_index == 0U &&
                 element.source_byte_offset == 0U &&
                 element.source_byte_length == kStartTag.size(),
                 "cross-record start tag keeps exact total span") ||
        !require(element.attributes.size() == 1U &&
                 element.attributes[0].name == "data-x" &&
                 element.attributes[0].value == "1",
                 "element attribute round trip") ||
        !require(text.tag == "#text", "text semantic") ||
        !require(text.parent_ordinal == 1U, "text parent is element ordinal") ||
        !require(text.source_record_index == 1U &&
                 text.source_byte_offset == std::string_view("ta-x='1'>hel").find("hel") &&
                 text.source_byte_length == kText.size(),
                 "text keeps one exact cross-record source span")) {
        return false;
    }

    LogicalNodeSourceV2ValidationStats validation;
    return require(validate_logical_node_source_v2_against_store(
               source_path, store_root, &validation, &error), error) &&
        require(validation.nodes_validated == 3U, "validator node count") &&
        require(validation.attributes_validated == 1U, "validator attribute count") &&
        require(validation.source_span_bytes_streamed ==
                    kStartTag.size() + kText.size(),
                "validator streams exact node-backed source bytes");
}

bool test_input_window_equivalence() {
    const std::filesystem::path root = unique_root("html-v2-window-equivalence");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    std::string error;
    if (!require(build_cross_record_store(store_root, &error), error)) {
        return false;
    }

    std::array<std::filesystem::path, 3> outputs{{
        root / "nodes-1.zvnsrc",
        root / "nodes-2.zvnsrc",
        root / "nodes-7.zvnsrc",
    }};
    constexpr std::array<std::size_t, 3> windows{{1U, 2U, 7U}};
    for (std::size_t index = 0U; index < outputs.size(); ++index) {
        StreamingHtmlNodeSourceConfig config;
        config.input_window_bytes = windows[index];
        if (!require(produce_streaming_html_node_source_v2(
                store_root, outputs[index], config, nullptr, &error), error)) {
            return false;
        }
    }
    const std::vector<std::byte> reference = read_file(outputs[0]);
    return require(!reference.empty(), "reference source is readable") &&
        require(read_file(outputs[1]) == reference, "2-byte input window is byte-identical") &&
        require(read_file(outputs[2]) == reference, "7-byte input window is byte-identical");
}

bool test_working_set_rejection_cleans_sidecar() {
    const std::filesystem::path root = unique_root("html-v2-working-set");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    std::string error;
    if (!require(build_cross_record_store(store_root, &error), error)) {
        return false;
    }
    StreamingHtmlNodeSourceConfig config;
    config.working_set_limit_bytes = 1U;
    StreamingHtmlNodeSourceV2Stats stats;
    return require(!produce_streaming_html_node_source_v2(
               store_root, source_path, config, &stats, &error),
               "1-byte parser budget rejects allocation") &&
        require(stats.working_set_rejected_reservations > 0U,
                "hard-cap rejection is observable") &&
        require(stats.working_set_current_bytes == 0U,
                "failed parser releases all reservations") &&
        require(!std::filesystem::exists(source_path),
                "failed parser does not publish source") &&
        require(!std::filesystem::exists(
                    std::filesystem::path(source_path.string() + ".building")),
                "failed parser removes building source");
}

bool test_special_text_state_remains_fail_closed() {
    const std::filesystem::path root = unique_root("html-v2-special-text");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    std::string error;
    if (!require(build_special_text_store(store_root, &error), error)) {
        return false;
    }
    return require(!produce_streaming_html_node_source_v2(
               store_root, source_path, {}, nullptr, &error),
               "script special tokenizer state remains unsupported") &&
        require(error.find("special HTML tokenizer state is not implemented") !=
                    std::string::npos,
                "special tokenizer failure is explicit") &&
        require(!std::filesystem::exists(source_path),
                "unsupported input cannot publish source");
}

} // namespace

int main() {
    if (!test_cross_record_markup_and_text_round_trip() ||
        !test_input_window_equivalence() ||
        !test_working_set_rejection_cleans_sidecar() ||
        !test_special_text_state_remains_fail_closed()) {
        return 1;
    }
    return 0;
}
