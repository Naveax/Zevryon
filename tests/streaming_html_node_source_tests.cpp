#include "logical_node_arena.hpp"
#include "logical_node_source.hpp"
#include "massivedoc_store.hpp"
#include "streaming_html_node_source.hpp"

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
#include <vector>

namespace {

using zevryon::massivedoc::CorpusMetadata;
using zevryon::massivedoc::LogicalNodeArenaReader;
using zevryon::massivedoc::LogicalNodeAttributeRecord;
using zevryon::massivedoc::LogicalNodeRecord;
using zevryon::massivedoc::LogicalNodeSourceImportConfig;
using zevryon::massivedoc::LogicalNodeSourceNode;
using zevryon::massivedoc::LogicalNodeSourceReader;
using zevryon::massivedoc::LogicalSemanticKind;
using zevryon::massivedoc::StoreWriter;
using zevryon::massivedoc::StreamingHtmlNodeSourceConfig;
using zevryon::massivedoc::StreamingHtmlNodeSourceStats;
using zevryon::massivedoc::import_logical_node_source_to_arena;
using zevryon::massivedoc::produce_streaming_html_node_source;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: streaming html node source: " << message << '\n';
        return false;
    }
    return true;
}

std::filesystem::path unique_root(std::string_view name) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (std::string("zevryon-") + std::string(name) + "-" +
         std::to_string(tick));
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
        reinterpret_cast<const std::byte*>(value.data()),
        value.size());
}

bool build_html_store(
    const std::filesystem::path& root,
    const std::vector<std::string_view>& records,
    std::uint64_t logical_nodes,
    std::string* error) {
    StoreWriter writer(root);
    std::uint64_t logical_bytes = 0U;
    std::uint64_t largest = 0U;
    for (std::size_t index = 0U; index < records.size(); ++index) {
        if (!writer.append(
                1000U + static_cast<std::uint64_t>(index),
                bytes(records[index]),
                error)) {
            return false;
        }
        logical_bytes += static_cast<std::uint64_t>(records[index].size());
        largest = std::max<std::uint64_t>(
            largest,
            static_cast<std::uint64_t>(records[index].size()));
    }
    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = logical_bytes;
    metadata.logical_records = static_cast<std::uint64_t>(records.size());
    metadata.logical_nodes = logical_nodes;
    metadata.largest_record_bytes = largest;
    return writer.finalize(metadata, nullptr, error);
}

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return {};
    }
    const std::streampos end = stream.tellg();
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
    if (!stream) {
        return {};
    }
    return output;
}

LogicalNodeSourceImportConfig import_config() {
    LogicalNodeSourceImportConfig config;
    config.candidate_commit =
        "0123456789abcdef0123456789abcdef01234567";
    config.candidate_tree =
        "89abcdef0123456789abcdef0123456789abcdef";
    config.semantic_bucket_count = 64U;
    config.semantic_hash_bits = 64U;
    return config;
}

bool next_node(
    LogicalNodeSourceReader* reader,
    LogicalNodeSourceNode* node,
    std::string* error) {
    bool has_node = false;
    return reader->next(node, &has_node, error) && has_node;
}

bool test_real_semantics_cross_record_and_arena_import() {
    const std::filesystem::path root = unique_root("html-producer-roundtrip");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    const std::vector<std::string_view> records{
        "<!doctype html><!--comment > still comment--><section role=\"ma&#105;n\" ",
        "style='display:&quot;block&quot;' id=x><div class=a disabled></div><br/></section>"};
    std::string error;
    if (!require(build_html_store(store_root, records, 4U, &error), error)) {
        return false;
    }

    StreamingHtmlNodeSourceConfig config;
    config.input_window_bytes = 3U;
    StreamingHtmlNodeSourceStats stats;
    if (!require(
            produce_streaming_html_node_source(
                store_root,
                source_path,
                config,
                &stats,
                &error),
            error) ||
        !require(stats.source_records == 2U, "two physical source records scanned") ||
        !require(stats.nodes_emitted == 4U, "document plus three elements emitted") ||
        !require(stats.attributes_emitted == 5U, "five element attributes emitted") ||
        !require(stats.comments_skipped == 1U, "comment skipped") ||
        !require(stats.doctypes_skipped == 1U, "doctype skipped") ||
        !require(stats.cross_record_token_anchors == 1U, "cross-record start tag anchored") ||
        !require(stats.maximum_observed_open_depth == 2U, "bounded depth observed")) {
        return false;
    }

    LogicalNodeSourceReader source(source_path);
    LogicalNodeSourceNode document;
    LogicalNodeSourceNode section;
    LogicalNodeSourceNode div;
    LogicalNodeSourceNode br;
    bool has_more = true;
    if (!require(source.open(&error), error) ||
        !require(next_node(&source, &document, &error), error) ||
        !require(next_node(&source, &section, &error), error) ||
        !require(next_node(&source, &div, &error), error) ||
        !require(next_node(&source, &br, &error), error) ||
        !require(source.next(&document, &has_more, &error), error) ||
        !require(!has_more, "source exhausted exactly at manifest node count") ||
        !require(document.tag == "#document", "real document node emitted") ||
        !require(section.tag == "section", "section tag parsed") ||
        !require(section.role == "main", "role entity decoded") ||
        !require(section.style == "display:\"block\"", "style entity decoded") ||
        !require(section.attributes.size() == 3U, "section attributes preserved") ||
        !require(section.source_record_index == 0U, "cross-record token anchors first record") ||
        !require(section.source_byte_length == 0U, "cross-record token uses zero-length safe anchor") ||
        !require(div.tag == "div" && div.parent_ordinal == 1U, "nested div topology") ||
        !require(br.tag == "br" && br.parent_ordinal == 1U, "void element topology")) {
        return false;
    }

    if (!require(
            import_logical_node_source_to_arena(
                source_path,
                store_root,
                import_config(),
                &error),
            error)) {
        return false;
    }
    LogicalNodeArenaReader arena(store_root);
    LogicalNodeRecord section_record;
    LogicalNodeRecord div_record;
    std::string semantic;
    if (!require(arena.open(&error), error) ||
        !require(arena.node_by_ordinal(1U, &section_record, &error), error) ||
        !require(arena.node_by_ordinal(2U, &div_record, &error), error) ||
        !require(section_record.first_child_ordinal == 2U, "arena first-child topology") ||
        !require(div_record.next_sibling_ordinal == 3U, "arena sibling topology") ||
        !require(
            arena.resolve_semantic(
                LogicalSemanticKind::role,
                section_record.role_id,
                &semantic,
                &error),
            error) ||
        !require(semantic == "main", "role reaches disk-backed arena")) {
        return false;
    }
    return true;
}

bool test_input_chunk_size_equivalence() {
    const std::filesystem::path root = unique_root("html-producer-chunk-equivalence");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path first_path = root / "one-byte.zvnsrc";
    const std::filesystem::path second_path = root / "wide.zvnsrc";
    const std::vector<std::string_view> records{
        "<!doctype html><main id=alpha><p class='x'>ignored text</p><img alt=photo></main>"};
    std::string error;
    if (!require(build_html_store(store_root, records, 5U, &error), error)) {
        return false;
    }

    StreamingHtmlNodeSourceConfig tiny;
    tiny.input_window_bytes = 1U;
    StreamingHtmlNodeSourceConfig wide;
    wide.input_window_bytes = 257U;
    if (!require(
            produce_streaming_html_node_source(
                store_root,
                first_path,
                tiny,
                nullptr,
                &error),
            error) ||
        !require(
            produce_streaming_html_node_source(
                store_root,
                second_path,
                wide,
                nullptr,
                &error),
            error)) {
        return false;
    }
    const std::vector<std::byte> first = read_file(first_path);
    const std::vector<std::byte> second = read_file(second_path);
    return require(!first.empty() && first == second, "source bytes are chunk-size invariant");
}

bool test_mismatched_end_tag_fails_closed() {
    const std::filesystem::path root = unique_root("html-producer-mismatch");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    const std::vector<std::string_view> records{"<main><div></main></div>"};
    std::string error;
    if (!require(build_html_store(store_root, records, 3U, &error), error) ||
        !require(
            !produce_streaming_html_node_source(
                store_root,
                source_path,
                {},
                nullptr,
                &error),
            "mismatched end tag rejected") ||
        !require(!std::filesystem::exists(source_path), "malformed HTML cannot publish source")) {
        return false;
    }
    std::filesystem::path building = source_path;
    building += ".building";
    return require(!std::filesystem::exists(building), "malformed HTML cleans building source");
}

bool test_raw_text_element_fails_closed() {
    const std::filesystem::path root = unique_root("html-producer-rawtext");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    const std::vector<std::string_view> records{"<main><script>if (a < b) x();</script></main>"};
    std::string error;
    if (!require(build_html_store(store_root, records, 3U, &error), error)) {
        return false;
    }
    return require(
        !produce_streaming_html_node_source(
            store_root,
            source_path,
            {},
            nullptr,
            &error),
        "unimplemented raw-text tokenizer state rejects instead of misparsing");
}

bool test_depth_bound_fails_closed() {
    const std::filesystem::path root = unique_root("html-producer-depth");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    const std::vector<std::string_view> records{"<a><b></b></a>"};
    std::string error;
    if (!require(build_html_store(store_root, records, 3U, &error), error)) {
        return false;
    }
    StreamingHtmlNodeSourceConfig config;
    config.maximum_open_element_depth = 1U;
    return require(
        !produce_streaming_html_node_source(
            store_root,
            source_path,
            config,
            nullptr,
            &error),
        "open-element depth cap enforced");
}

bool test_token_bound_and_duplicate_attribute_fail_closed() {
    const std::filesystem::path root = unique_root("html-producer-bounds");
    RootCleanup cleanup(root);
    std::string error;

    const std::filesystem::path token_store = root / "token-store";
    const std::filesystem::path token_source = root / "token.zvnsrc";
    if (!require(
            build_html_store(token_store, {"<abcdefgh></abcdefgh>"}, 2U, &error),
            error)) {
        return false;
    }
    StreamingHtmlNodeSourceConfig token_config;
    token_config.maximum_token_bytes = 8U;
    if (!require(
            !produce_streaming_html_node_source(
                token_store,
                token_source,
                token_config,
                nullptr,
                &error),
            "oversized markup token rejected")) {
        return false;
    }

    const std::filesystem::path duplicate_store = root / "duplicate-store";
    const std::filesystem::path duplicate_source = root / "duplicate.zvnsrc";
    if (!require(
            build_html_store(duplicate_store, {"<main ID=a id=b></main>"}, 2U, &error),
            error) ||
        !require(
            !produce_streaming_html_node_source(
                duplicate_store,
                duplicate_source,
                {},
                nullptr,
                &error),
            "case-folded duplicate attribute rejected")) {
        return false;
    }
    return true;
}

bool test_envelope_node_count_mismatch_rejected() {
    const std::filesystem::path root = unique_root("html-producer-envelope");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    std::string error;
    if (!require(
            build_html_store(store_root, {"<main></main>"}, 99U, &error),
            error) ||
        !require(
            !produce_streaming_html_node_source(
                store_root,
                source_path,
                {},
                nullptr,
                &error),
            "logical_nodes envelope cannot certify invented nodes") ||
        !require(!std::filesystem::exists(source_path), "count mismatch cannot publish source")) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!test_real_semantics_cross_record_and_arena_import() ||
        !test_input_chunk_size_equivalence() ||
        !test_mismatched_end_tag_fails_closed() ||
        !test_raw_text_element_fails_closed() ||
        !test_depth_bound_fails_closed() ||
        !test_token_bound_and_duplicate_attribute_fail_closed() ||
        !test_envelope_node_count_mismatch_rejected()) {
        return 1;
    }
    return 0;
}
