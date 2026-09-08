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

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: streaming HTML v2 strict syntax: " << message << '\n';
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

bool build_store_records(
    const std::filesystem::path& root,
    const std::vector<std::string_view>& records,
    std::uint64_t logical_nodes,
    std::string* error) {
    StoreWriter writer(root);
    std::uint64_t logical_utf8_bytes = 0U;
    std::uint64_t largest_record_bytes = 0U;
    for (std::size_t index = 0U; index < records.size(); ++index) {
        const std::string_view record = records[index];
        if (!writer.append(
                8301U + static_cast<std::uint64_t>(index),
                bytes(record),
                error)) {
            return false;
        }
        const auto record_bytes = static_cast<std::uint64_t>(record.size());
        if (logical_utf8_bytes >
            std::numeric_limits<std::uint64_t>::max() - record_bytes) {
            if (error != nullptr) {
                *error = "test corpus byte count overflows";
            }
            return false;
        }
        logical_utf8_bytes += record_bytes;
        largest_record_bytes = std::max(largest_record_bytes, record_bytes);
    }

    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = logical_utf8_bytes;
    metadata.logical_records = static_cast<std::uint64_t>(records.size());
    metadata.logical_nodes = logical_nodes;
    metadata.largest_record_bytes = largest_record_bytes;
    return writer.finalize(metadata, nullptr, error);
}

bool build_store(
    const std::filesystem::path& root,
    std::string_view html,
    std::uint64_t logical_nodes,
    std::string* error) {
    return build_store_records(root, {html}, logical_nodes, error);
}

bool read_three_nodes(
    const std::filesystem::path& source_path,
    LogicalNodeSourceNode* document,
    LogicalNodeSourceNode* element,
    LogicalNodeSourceNode* text,
    std::string* error) {
    LogicalNodeSourceV2Reader reader(source_path);
    LogicalNodeSourceNode extra;
    bool has_node = false;
    return require(reader.open(error), *error) &&
        require(reader.next(document, &has_node, error) && has_node,
                "document node is present") &&
        require(reader.next(element, &has_node, error) && has_node,
                "element node is present") &&
        require(reader.next(text, &has_node, error) && has_node,
                "text node is present") &&
        require(reader.next(&extra, &has_node, error) && !has_node,
                "source contains exactly three nodes");
}

bool test_non_void_self_closing_syntax_fails_closed() {
    const std::filesystem::path root = unique_root("html-v2-nonvoid-self-close");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    std::string error;
    if (!require(build_store(store_root, "<div/>", 2U, &error), error)) {
        return false;
    }

    return require(!produce_streaming_html_node_source_v2(
                       store_root, source_path, {}, nullptr, &error),
                   "non-void self-closing syntax is rejected") &&
        require(error.find("self-closing syntax on non-void") != std::string::npos,
                "rejection identifies unsupported non-void self-closing syntax") &&
        require(!std::filesystem::exists(source_path),
                "rejected non-void syntax cannot publish source") &&
        require(!std::filesystem::exists(
                    std::filesystem::path(source_path.string() + ".building")),
                "rejected non-void syntax leaves no building source");
}

bool test_void_self_closing_syntax_remains_supported() {
    const std::filesystem::path root = unique_root("html-v2-void-self-close");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    std::string error;
    if (!require(build_store(store_root, "<br/>", 2U, &error), error) ||
        !require(produce_streaming_html_node_source_v2(
                     store_root, source_path, {}, nullptr, &error), error)) {
        return false;
    }

    LogicalNodeSourceV2Reader reader(source_path);
    LogicalNodeSourceNode document;
    LogicalNodeSourceNode element;
    LogicalNodeSourceNode extra;
    bool has_node = false;
    if (!require(reader.open(&error), error) ||
        !require(reader.next(&document, &has_node, &error) && has_node,
                 "document node is present") ||
        !require(reader.next(&element, &has_node, &error) && has_node,
                 "void element node is present") ||
        !require(reader.next(&extra, &has_node, &error) && !has_node,
                 "source contains exactly two nodes") ||
        !require(document.tag == "#document", "document semantic survives") ||
        !require(element.tag == "br", "void tag survives") ||
        !require(element.parent_ordinal == 0U, "void element parent survives") ||
        !require(element.source_record_index == 0U &&
                     element.source_byte_offset == 0U &&
                     element.source_byte_length == 5U,
                 "void element keeps exact source span")) {
        return false;
    }

    LogicalNodeSourceV2ValidationStats validation;
    return require(validate_logical_node_source_v2_against_store(
                       source_path, store_root, &validation, &error), error) &&
        require(validation.nodes_validated == 2U, "validator accepts document + br") &&
        require(validation.source_span_bytes_streamed == 5U,
                "validator streams exact void-element source span");
}

bool test_style_rawtext_preserves_markup_like_bytes_as_one_text_span() {
    const std::filesystem::path root = unique_root("html-v2-style-rawtext");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    constexpr std::string_view raw_text = "a<b{color:red}";
    const std::string html = "<style>" + std::string(raw_text) + "</style>";
    std::string error;
    StreamingHtmlNodeSourceV2Stats stats;
    if (!require(build_store(store_root, html, 3U, &error), error) ||
        !require(produce_streaming_html_node_source_v2(
                     store_root, source_path, {}, &stats, &error), error)) {
        return false;
    }

    LogicalNodeSourceNode document;
    LogicalNodeSourceNode style;
    LogicalNodeSourceNode text;
    if (!read_three_nodes(source_path, &document, &style, &text, &error) ||
        !require(document.tag == "#document", "style document semantic survives") ||
        !require(style.tag == "style" && style.parent_ordinal == 0U,
                 "style element semantic survives") ||
        !require(style.source_record_index == 0U &&
                     style.source_byte_offset == 0U &&
                     style.source_byte_length == 7U,
                 "style element keeps exact start-tag source span") ||
        !require(text.tag == "#text" && text.parent_ordinal == 1U,
                 "style RAWTEXT is emitted under the style element") ||
        !require(text.source_record_index == 0U &&
                     text.source_byte_offset == 7U &&
                     text.source_byte_length == raw_text.size(),
                 "markup-like style bytes remain one exact text span") ||
        !require(stats.element_nodes_emitted == 1U && stats.text_nodes_emitted == 1U,
                 "style RAWTEXT emits one element and one text node")) {
        return false;
    }

    LogicalNodeSourceV2ValidationStats validation;
    return require(validate_logical_node_source_v2_against_store(
                       source_path, store_root, &validation, &error), error) &&
        require(validation.nodes_validated == 3U,
                "validator accepts document + style + RAWTEXT") &&
        require(validation.source_span_bytes_streamed == 7U + raw_text.size(),
                "validator streams exact style start-tag and RAWTEXT spans");
}

bool test_style_rawtext_false_end_tag_candidate_stays_text() {
    const std::filesystem::path root = unique_root("html-v2-style-false-close");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    constexpr std::string_view raw_text = "a</stylex><b{c:d}";
    const std::string html = "<style>" + std::string(raw_text) + "</style>";
    std::string error;
    if (!require(build_store(store_root, html, 3U, &error), error) ||
        !require(produce_streaming_html_node_source_v2(
                     store_root, source_path, {}, nullptr, &error), error)) {
        return false;
    }

    LogicalNodeSourceNode document;
    LogicalNodeSourceNode style;
    LogicalNodeSourceNode text;
    return read_three_nodes(source_path, &document, &style, &text, &error) &&
        require(text.tag == "#text" && text.parent_ordinal == 1U,
                "false style close candidate remains style text") &&
        require(text.source_record_index == 0U &&
                    text.source_byte_offset == 7U &&
                    text.source_byte_length == raw_text.size(),
                "false close and following markup-like bytes stay in one span");
}

bool test_style_rawtext_close_can_cross_records_and_one_byte_windows() {
    const std::filesystem::path root = unique_root("html-v2-style-cross-record-close");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    const std::vector<std::string_view> records = {
        "<style>a",
        "{b:c}</ST",
        "YLE \t>"};
    std::string error;
    if (!require(build_store_records(store_root, records, 3U, &error), error)) {
        return false;
    }

    StreamingHtmlNodeSourceConfig config;
    config.input_window_bytes = 1U;
    StreamingHtmlNodeSourceV2Stats stats;
    if (!require(produce_streaming_html_node_source_v2(
                     store_root, source_path, config, &stats, &error), error)) {
        return false;
    }

    LogicalNodeSourceNode document;
    LogicalNodeSourceNode style;
    LogicalNodeSourceNode text;
    if (!read_three_nodes(source_path, &document, &style, &text, &error) ||
        !require(text.tag == "#text" && text.parent_ordinal == 1U,
                 "cross-record style text keeps style parent") ||
        !require(text.source_record_index == 0U &&
                     text.source_byte_offset == 7U &&
                     text.source_byte_length == 6U,
                 "cross-record RAWTEXT source span is exact") ||
        !require(stats.cross_record_text_spans == 1U,
                 "cross-record RAWTEXT is accounted exactly once")) {
        return false;
    }

    LogicalNodeSourceV2ValidationStats validation;
    return require(validate_logical_node_source_v2_against_store(
                       source_path, store_root, &validation, &error), error) &&
        require(validation.nodes_validated == 3U,
                "validator accepts cross-record style RAWTEXT");
}

bool test_script_remains_fail_closed() {
    const std::filesystem::path root = unique_root("html-v2-script-still-unsupported");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    std::string error;
    if (!require(build_store(store_root, "<script>x</script>", 3U, &error), error)) {
        return false;
    }

    return require(!produce_streaming_html_node_source_v2(
                       store_root, source_path, {}, nullptr, &error),
                   "script tokenizer state remains rejected") &&
        require(error.find("raw-text HTML element is not implemented") != std::string::npos,
                "script rejection remains explicit") &&
        require(!std::filesystem::exists(source_path),
                "rejected script input cannot publish source") &&
        require(!std::filesystem::exists(
                    std::filesystem::path(source_path.string() + ".building")),
                "rejected script input leaves no building source");
}

} // namespace

int main() {
    if (!test_non_void_self_closing_syntax_fails_closed() ||
        !test_void_self_closing_syntax_remains_supported() ||
        !test_style_rawtext_preserves_markup_like_bytes_as_one_text_span() ||
        !test_style_rawtext_false_end_tag_candidate_stays_text() ||
        !test_style_rawtext_close_can_cross_records_and_one_byte_windows() ||
        !test_script_remains_fail_closed()) {
        return 1;
    }
    return 0;
}
