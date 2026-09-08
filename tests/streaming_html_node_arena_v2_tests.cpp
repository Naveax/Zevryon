#include "massivedoc_store.hpp"
#include "streaming_html_node_arena_v2.hpp"
#include "zenith_semantic_runtime_consumer.hpp"

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

namespace {

using zevryon::massivedoc::CorpusMetadata;
using zevryon::massivedoc::StoreWriter;
using zevryon::massivedoc::StreamingHtmlNodeArenaV2Config;
using zevryon::massivedoc::StreamingHtmlNodeArenaV2Stats;
using zevryon::massivedoc::ZenithSemanticNodeWindowResult;
using zevryon::massivedoc::ZenithSemanticRuntimeConsumer;
using zevryon::massivedoc::build_streaming_html_node_arena_v2;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: streaming HTML node arena v2: " << message << '\n';
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

bool build_store(
    const std::filesystem::path& store_root,
    std::string_view html,
    std::uint64_t logical_nodes,
    std::string* error) {
    StoreWriter writer(store_root);
    if (!writer.append(9301U, bytes(html), error)) {
        return false;
    }
    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = static_cast<std::uint64_t>(html.size());
    metadata.logical_records = 1U;
    metadata.logical_nodes = logical_nodes;
    metadata.largest_record_bytes = static_cast<std::uint64_t>(html.size());
    return writer.finalize(metadata, nullptr, error);
}

StreamingHtmlNodeArenaV2Config pipeline_config() {
    StreamingHtmlNodeArenaV2Config config;
    config.import.candidate_commit =
        "0123456789abcdef0123456789abcdef01234567";
    config.import.candidate_tree =
        "89abcdef0123456789abcdef0123456789abcdef";
    config.import.semantic_bucket_count = 64U;
    config.import.semantic_hash_bits = 64U;
    return config;
}

bool test_end_to_end_parser_import_runtime_consumer() {
    const std::filesystem::path root = unique_root("html-v2-arena-roundtrip");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    constexpr std::string_view raw_text = "a&amp;<b";
    const std::string html = "<title>" + std::string(raw_text) + "</title>";
    std::string error;
    if (!require(build_store(store_root, html, 3U, &error), error)) {
        return false;
    }

    StreamingHtmlNodeArenaV2Stats stats;
    if (!require(build_streaming_html_node_arena_v2(
                     store_root,
                     source_path,
                     pipeline_config(),
                     &stats,
                     &error),
                 error) ||
        !require(stats.source_published, "authoritative source remains published") ||
        !require(stats.arena_published, "store-bound arena remains published") ||
        !require(stats.parser.nodes_emitted == 3U, "parser emits exact node count") ||
        !require(stats.import.nodes_imported == 3U, "importer replays exact node count") ||
        !require(std::filesystem::exists(source_path), "source artifact exists") ||
        !require(std::filesystem::exists(store_root / "node-arena-v2"),
                 "arena artifact exists") ||
        !require(!std::filesystem::exists(store_root / "node-arena-v2.building"),
                 "arena staging is absent after success")) {
        return false;
    }

    ZenithSemanticRuntimeConsumer consumer(store_root);
    ZenithSemanticNodeWindowResult result;
    if (!require(consumer.read(0U, &result, &error), error) ||
        !require(result.nodes.size() == 3U, "runtime reads document, title and text") ||
        !require(result.nodes[0].tag == "#document", "runtime document semantic") ||
        !require(result.nodes[1].tag == "title", "runtime title semantic") ||
        !require(result.nodes[2].tag == "#text", "runtime text semantic") ||
        !require(result.nodes[1].record.parent_ordinal == 0U,
                 "runtime title parent topology") ||
        !require(result.nodes[2].record.parent_ordinal == 1U,
                 "runtime text parent topology") ||
        !require(result.nodes[2].record.source_record_index == 0U &&
                     result.nodes[2].record.source_byte_offset == 7U &&
                     result.nodes[2].record.source_byte_length == raw_text.size(),
                 "runtime preserves exact raw RCDATA source identity")) {
        return false;
    }

    const auto runtime_stats = consumer.stats();
    return require(runtime_stats.successful_windows == 1U,
                   "runtime consumer records successful production read") &&
        require(runtime_stats.materialized_nodes == 3U,
                "runtime consumer accounts exact materialized nodes");
}

bool test_import_failure_rolls_back_owned_source() {
    const std::filesystem::path root = unique_root("html-v2-arena-import-rollback");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    constexpr std::string_view html = "<style>x</style>";
    std::string error;
    if (!require(build_store(store_root, html, 3U, &error), error)) {
        return false;
    }

    StreamingHtmlNodeArenaV2Config config = pipeline_config();
    config.import.candidate_commit = "invalid";
    StreamingHtmlNodeArenaV2Stats stats;
    return require(!build_streaming_html_node_arena_v2(
                       store_root, source_path, config, &stats, &error),
                   "invalid import config fails the combined pipeline") &&
        require(error.find("configuration is invalid") != std::string::npos,
                "import failure reason survives rollback") &&
        require(!stats.source_published,
                "owned source publication is rolled back after import failure") &&
        require(!stats.arena_published, "failed import never reports arena publication") &&
        require(stats.parser.nodes_emitted == 3U,
                "parser success before import failure remains observable") &&
        require(!std::filesystem::exists(source_path),
                "failed combined pipeline removes its published source") &&
        require(!std::filesystem::exists(
                    std::filesystem::path(source_path.string() + ".building")),
                "failed combined pipeline leaves no source staging") &&
        require(!std::filesystem::exists(store_root / "node-arena-v2"),
                "failed combined pipeline cannot publish arena") &&
        require(!std::filesystem::exists(store_root / "node-arena-v2.building"),
                "failed combined pipeline leaves no arena staging");
}

bool test_parser_failure_never_arms_source_rollback() {
    const std::filesystem::path root = unique_root("html-v2-arena-parser-failure");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    constexpr std::string_view html = "<script>x</script>";
    std::string error;
    if (!require(build_store(store_root, html, 3U, &error), error)) {
        return false;
    }

    StreamingHtmlNodeArenaV2Stats stats;
    return require(!build_streaming_html_node_arena_v2(
                       store_root,
                       source_path,
                       pipeline_config(),
                       &stats,
                       &error),
                   "unsupported parser state fails before import") &&
        require(error.find("special HTML tokenizer state is not implemented") !=
                    std::string::npos,
                "parser failure remains explicit") &&
        require(!stats.source_published && !stats.arena_published,
                "parser failure publishes neither pipeline artifact") &&
        require(!std::filesystem::exists(source_path),
                "parser failure leaves no source artifact") &&
        require(!std::filesystem::exists(store_root / "node-arena-v2"),
                "parser failure leaves no arena artifact");
}

bool test_preexisting_source_is_never_deleted() {
    const std::filesystem::path root = unique_root("html-v2-arena-preexisting-source");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    constexpr std::string_view html = "<style>x</style>";
    std::string error;
    if (!require(build_store(store_root, html, 3U, &error), error)) {
        return false;
    }
    {
        std::ofstream sentinel(source_path, std::ios::binary | std::ios::trunc);
        sentinel << "sentinel";
        if (!require(static_cast<bool>(sentinel), "preexisting source fixture written")) {
            return false;
        }
    }

    StreamingHtmlNodeArenaV2Stats stats;
    if (!require(!build_streaming_html_node_arena_v2(
                     store_root,
                     source_path,
                     pipeline_config(),
                     &stats,
                     &error),
                 "create-only parser rejects preexisting source") ||
        !require(!stats.source_published && !stats.arena_published,
                 "preexisting source never arms pipeline rollback") ||
        !require(std::filesystem::exists(source_path),
                 "preexisting source survives pipeline failure")) {
        return false;
    }

    std::ifstream sentinel(source_path, std::ios::binary);
    std::string contents;
    sentinel >> contents;
    return require(contents == "sentinel",
                   "preexisting source contents remain untouched");
}

bool test_preexisting_arena_is_preserved_and_retry_source_rolls_back() {
    const std::filesystem::path root = unique_root("html-v2-arena-preexisting-arena");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path first_source = root / "first.zvnsrc";
    const std::filesystem::path retry_source = root / "retry.zvnsrc";
    constexpr std::string_view html = "<style>x</style>";
    std::string error;
    if (!require(build_store(store_root, html, 3U, &error), error) ||
        !require(build_streaming_html_node_arena_v2(
                     store_root,
                     first_source,
                     pipeline_config(),
                     nullptr,
                     &error),
                 error)) {
        return false;
    }

    StreamingHtmlNodeArenaV2Stats retry_stats;
    if (!require(!build_streaming_html_node_arena_v2(
                     store_root,
                     retry_source,
                     pipeline_config(),
                     &retry_stats,
                     &error),
                 "existing authoritative arena rejects replacement pipeline") ||
        !require(!retry_stats.source_published && !retry_stats.arena_published,
                 "retry failure reports no newly published pipeline artifact") ||
        !require(std::filesystem::exists(first_source),
                 "first authoritative source remains published") ||
        !require(!std::filesystem::exists(retry_source),
                 "retry source is rolled back after arena publication rejection") ||
        !require(std::filesystem::exists(store_root / "node-arena-v2"),
                 "existing authoritative arena remains published") ||
        !require(!std::filesystem::exists(store_root / "node-arena-v2.building"),
                 "retry leaves no arena staging")) {
        return false;
    }

    ZenithSemanticRuntimeConsumer consumer(store_root);
    ZenithSemanticNodeWindowResult result;
    return require(consumer.read(0U, &result, &error), error) &&
        require(result.nodes.size() == 3U,
                "existing authoritative arena remains runtime-readable after retry");
}

bool test_source_artifact_inside_store_is_rejected_before_parser() {
    const std::filesystem::path root = unique_root("html-v2-arena-store-isolation");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = store_root / "pipeline-nodes.zvnsrc";
    constexpr std::string_view html = "<style>x</style>";
    std::string error;
    if (!require(build_store(store_root, html, 3U, &error), error)) {
        return false;
    }

    StreamingHtmlNodeArenaV2Stats stats;
    return require(!build_streaming_html_node_arena_v2(
                       store_root,
                       source_path,
                       pipeline_config(),
                       &stats,
                       &error),
                   "source artifact inside native store is rejected") &&
        require(error.find("must reside outside the authoritative native store tree") !=
                    std::string::npos,
                "store-tree isolation failure is explicit") &&
        require(stats.parser.nodes_emitted == 0U,
                "store-tree isolation rejects before parser execution") &&
        require(!stats.source_published && !stats.arena_published,
                "store-tree isolation publishes no pipeline artifact") &&
        require(!std::filesystem::exists(source_path),
                "store-tree isolation does not mutate authoritative store") &&
        require(!std::filesystem::exists(
                    std::filesystem::path(source_path.string() + ".building")),
                "store-tree isolation leaves no source staging") &&
        require(!std::filesystem::exists(store_root / "node-arena-v2"),
                "store-tree isolation cannot publish arena");
}

} // namespace

int main() {
    if (!test_end_to_end_parser_import_runtime_consumer() ||
        !test_import_failure_rolls_back_owned_source() ||
        !test_parser_failure_never_arms_source_rollback() ||
        !test_preexisting_source_is_never_deleted() ||
        !test_preexisting_arena_is_preserved_and_retry_source_rolls_back() ||
        !test_source_artifact_inside_store_is_rejected_before_parser()) {
        return 1;
    }
    return 0;
}
