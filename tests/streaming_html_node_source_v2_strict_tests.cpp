#include "logical_node_source_v2.hpp"
#include "massivedoc_store.hpp"
#include "streaming_html_node_source_v2.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
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
    if (!reader.open(error)) {
        return require(false, error == nullptr ? "reader open failed" : *error);
    }
    return require(reader.next(document, &has_node, error) && has_node,
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

bool test_rawtext_element(std::string_view tag) {
    const std::filesystem::path root = unique_root(
        std::string("html-v2-rawtext-") + std::string(tag));
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    constexpr std::string_view raw_text = "a<b{color:red}";
    const std::string start_tag = "<" + std::string(tag) + ">";
    const std::string html = start_tag + std::string(raw_text) +
        "</" + std::string(tag) + ">";
    std::string error;
    StreamingHtmlNodeSourceV2Stats stats;
    if (!require(build_store(store_root, html, 3U, &error), error) ||
        !require(produce_streaming_html_node_source_v2(
                     store_root, source_path, {}, &stats, &error), error)) {
        return false;
    }

    LogicalNodeSourceNode document;
    LogicalNodeSourceNode element;
    LogicalNodeSourceNode text;
    if (!read_three_nodes(source_path, &document, &element, &text, &error) ||
        !require(document.tag == "#document", "RAWTEXT document semantic survives") ||
        !require(element.tag == tag && element.parent_ordinal == 0U,
                 "RAWTEXT element semantic survives") ||
        !require(element.source_record_index == 0U &&
                     element.source_byte_offset == 0U &&
                     element.source_byte_length == start_tag.size(),
                 "RAWTEXT element keeps exact start-tag source span") ||
        !require(text.tag == "#text" && text.parent_ordinal == 1U,
                 "RAWTEXT text is emitted under its element") ||
        !require(text.source_record_index == 0U &&
                     text.source_byte_offset == start_tag.size() &&
                     text.source_byte_length == raw_text.size(),
                 "markup-like RAWTEXT bytes remain one exact text span") ||
        !require(stats.element_nodes_emitted == 1U && stats.text_nodes_emitted == 1U,
                 "RAWTEXT emits one element and one text node")) {
        return false;
    }

    LogicalNodeSourceV2ValidationStats validation;
    return require(validate_logical_node_source_v2_against_store(
                       source_path, store_root, &validation, &error), error) &&
        require(validation.nodes_validated == 3U,
                "validator accepts document + RAWTEXT element + text") &&
        require(validation.source_span_bytes_streamed ==
                    start_tag.size() + raw_text.size(),
                "validator streams exact RAWTEXT start-tag and text spans");
}

bool test_rawtext_family_preserves_markup_like_bytes() {
    constexpr std::array<std::string_view, 5> tags{{
        "style", "xmp", "iframe", "noembed", "noframes"}};
    for (const std::string_view tag : tags) {
        if (!test_rawtext_element(tag)) {
            return false;
        }
    }
    return true;
}

bool test_rawtext_false_end_tag_candidate_stays_text() {
    const std::filesystem::path root = unique_root("html-v2-iframe-false-close");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    constexpr std::string_view raw_text = "a</iframex><b{c:d}";
    const std::string html = "<iframe>" + std::string(raw_text) + "</iframe>";
    std::string error;
    if (!require(build_store(store_root, html, 3U, &error), error) ||
        !require(produce_streaming_html_node_source_v2(
                     store_root, source_path, {}, nullptr, &error), error)) {
        return false;
    }

    LogicalNodeSourceNode document;
    LogicalNodeSourceNode iframe;
    LogicalNodeSourceNode text;
    return read_three_nodes(source_path, &document, &iframe, &text, &error) &&
        require(iframe.tag == "iframe", "iframe RAWTEXT element survives") &&
        require(text.tag == "#text" && text.parent_ordinal == 1U,
                "false iframe close candidate remains text") &&
        require(text.source_record_index == 0U &&
                    text.source_byte_offset == 8U &&
                    text.source_byte_length == raw_text.size(),
                "false close and following markup-like bytes stay in one span");
}

bool test_rawtext_close_can_cross_records_and_one_byte_windows() {
    const std::filesystem::path root = unique_root("html-v2-noframes-cross-record-close");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    const std::vector<std::string_view> records = {
        "<noframes>a",
        "{b:c}</NOF",
        "RAMES \t>"};
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
    LogicalNodeSourceNode noframes;
    LogicalNodeSourceNode text;
    if (!read_three_nodes(source_path, &document, &noframes, &text, &error) ||
        !require(noframes.tag == "noframes", "cross-record RAWTEXT tag survives") ||
        !require(text.tag == "#text" && text.parent_ordinal == 1U,
                 "cross-record RAWTEXT keeps element parent") ||
        !require(text.source_record_index == 0U &&
                     text.source_byte_offset == 10U &&
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
                "validator accepts cross-record RAWTEXT family state");
}

bool test_title_rcdata_preserves_raw_source_span() {
    const std::filesystem::path root = unique_root("html-v2-title-rcdata");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    constexpr std::string_view raw_text = "a&amp;<b{c:d}";
    const std::string html = "<title>" + std::string(raw_text) + "</title>";
    std::string error;
    if (!require(build_store(store_root, html, 3U, &error), error) ||
        !require(produce_streaming_html_node_source_v2(
                     store_root, source_path, {}, nullptr, &error), error)) {
        return false;
    }

    LogicalNodeSourceNode document;
    LogicalNodeSourceNode title;
    LogicalNodeSourceNode text;
    if (!read_three_nodes(source_path, &document, &title, &text, &error) ||
        !require(title.tag == "title" && title.parent_ordinal == 0U,
                 "title RCDATA element survives") ||
        !require(text.tag == "#text" && text.parent_ordinal == 1U,
                 "title RCDATA emits one text node") ||
        !require(text.source_record_index == 0U &&
                     text.source_byte_offset == 7U &&
                     text.source_byte_length == raw_text.size(),
                 "title RCDATA keeps raw character-reference and markup-like bytes in one source span")) {
        return false;
    }

    LogicalNodeSourceV2ValidationStats validation;
    return require(validate_logical_node_source_v2_against_store(
                       source_path, store_root, &validation, &error), error) &&
        require(validation.nodes_validated == 3U,
                "validator accepts title RCDATA structural source");
}

bool test_rcdata_false_end_tag_candidate_stays_text() {
    const std::filesystem::path root = unique_root("html-v2-title-false-close");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    constexpr std::string_view raw_text = "a</titlex><b";
    const std::string html = "<title>" + std::string(raw_text) + "</title>";
    std::string error;
    if (!require(build_store(store_root, html, 3U, &error), error) ||
        !require(produce_streaming_html_node_source_v2(
                     store_root, source_path, {}, nullptr, &error), error)) {
        return false;
    }

    LogicalNodeSourceNode document;
    LogicalNodeSourceNode title;
    LogicalNodeSourceNode text;
    return read_three_nodes(source_path, &document, &title, &text, &error) &&
        require(text.source_record_index == 0U &&
                    text.source_byte_offset == 7U &&
                    text.source_byte_length == raw_text.size(),
                "false title close candidate remains one RCDATA source span");
}

bool test_rcdata_close_can_cross_records_and_one_byte_windows() {
    const std::filesystem::path root = unique_root("html-v2-title-cross-record-close");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    const std::vector<std::string_view> records = {
        "<title>a",
        "&amp;</TI",
        "TLE \t>"};
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
    LogicalNodeSourceNode title;
    LogicalNodeSourceNode text;
    if (!read_three_nodes(source_path, &document, &title, &text, &error) ||
        !require(text.source_record_index == 0U &&
                     text.source_byte_offset == 7U &&
                     text.source_byte_length == 6U,
                 "cross-record RCDATA source span is exact") ||
        !require(stats.cross_record_text_spans == 1U,
                 "cross-record RCDATA is accounted exactly once")) {
        return false;
    }

    LogicalNodeSourceV2ValidationStats validation;
    return require(validate_logical_node_source_v2_against_store(
                       source_path, store_root, &validation, &error), error) &&
        require(validation.nodes_validated == 3U,
                "validator accepts cross-record RCDATA state");
}

bool test_textarea_initial_literal_lf_is_ignored() {
    const std::filesystem::path root = unique_root("html-v2-textarea-leading-lf");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    constexpr std::string_view html = "<textarea>\nhello</textarea>";
    std::string error;
    if (!require(build_store(store_root, html, 3U, &error), error) ||
        !require(produce_streaming_html_node_source_v2(
                     store_root, source_path, {}, nullptr, &error), error)) {
        return false;
    }

    LogicalNodeSourceNode document;
    LogicalNodeSourceNode textarea;
    LogicalNodeSourceNode text;
    return read_three_nodes(source_path, &document, &textarea, &text, &error) &&
        require(textarea.tag == "textarea", "textarea RCDATA element survives") &&
        require(text.source_record_index == 0U &&
                    text.source_byte_offset == 11U &&
                    text.source_byte_length == 5U,
                "textarea leading LF is excluded from the browser text-node source span");
}

bool test_textarea_initial_cr_remains_fail_closed() {
    const std::filesystem::path root = unique_root("html-v2-textarea-leading-cr");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    constexpr std::string_view html = "<textarea>\rhello</textarea>";
    std::string error;
    if (!require(build_store(store_root, html, 3U, &error), error)) {
        return false;
    }

    return require(!produce_streaming_html_node_source_v2(
                       store_root, source_path, {}, nullptr, &error),
                   "textarea initial CR is rejected until preprocessing is implemented") &&
        require(error.find("textarea CR/CRLF preprocessing is not implemented") !=
                    std::string::npos,
                "textarea CR rejection is explicit") &&
        require(!std::filesystem::exists(source_path),
                "rejected textarea CR input cannot publish source") &&
        require(!std::filesystem::exists(
                    std::filesystem::path(source_path.string() + ".building")),
                "rejected textarea CR input leaves no building source");
}

bool test_unimplemented_special_tokenizer_states_remain_fail_closed() {
    constexpr std::array<std::string_view, 3> tags{{
        "script", "plaintext", "noscript"}};
    for (const std::string_view tag : tags) {
        const std::filesystem::path root = unique_root(
            std::string("html-v2-special-") + std::string(tag));
        RootCleanup cleanup(root);
        const std::filesystem::path store_root = root / "store";
        const std::filesystem::path source_path = root / "nodes.zvnsrc";
        const std::string html = "<" + std::string(tag) + ">x</" +
            std::string(tag) + ">";
        std::string error;
        if (!require(build_store(store_root, html, 3U, &error), error)) {
            return false;
        }
        if (!require(!produce_streaming_html_node_source_v2(
                         store_root, source_path, {}, nullptr, &error),
                     "unimplemented special tokenizer state is rejected") ||
            !require(error.find("special HTML tokenizer state is not implemented") !=
                         std::string::npos,
                     "special tokenizer rejection remains explicit") ||
            !require(!std::filesystem::exists(source_path),
                     "rejected special tokenizer input cannot publish source") ||
            !require(!std::filesystem::exists(
                         std::filesystem::path(source_path.string() + ".building")),
                     "rejected special tokenizer input leaves no building source")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    if (!test_non_void_self_closing_syntax_fails_closed() ||
        !test_void_self_closing_syntax_remains_supported() ||
        !test_rawtext_family_preserves_markup_like_bytes() ||
        !test_rawtext_false_end_tag_candidate_stays_text() ||
        !test_rawtext_close_can_cross_records_and_one_byte_windows() ||
        !test_title_rcdata_preserves_raw_source_span() ||
        !test_rcdata_false_end_tag_candidate_stays_text() ||
        !test_rcdata_close_can_cross_records_and_one_byte_windows() ||
        !test_textarea_initial_literal_lf_is_ignored() ||
        !test_textarea_initial_cr_remains_fail_closed() ||
        !test_unimplemented_special_tokenizer_states_remain_fail_closed()) {
        return 1;
    }
    return 0;
}
