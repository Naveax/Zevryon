#include "massivedoc_store.hpp"
#include "streaming_html_node_source.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace {

using zevryon::massivedoc::CorpusMetadata;
using zevryon::massivedoc::StoreStats;
using zevryon::massivedoc::StoreWriter;
using zevryon::massivedoc::StreamingHtmlNodeSourceConfig;
using zevryon::massivedoc::StreamingHtmlNodeSourceStats;
using zevryon::massivedoc::produce_streaming_html_node_source;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: streaming HTML resource accounting: " << message << '\n';
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
    if (!writer.append(1001U, bytes(html), error)) {
        return false;
    }

    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = html.size();
    metadata.logical_records = 1U;
    metadata.logical_nodes = logical_nodes;
    metadata.style_runs = 0U;
    metadata.largest_record_bytes = html.size();
    StoreStats stats;
    return writer.finalize(metadata, &stats, error);
}

bool test_successful_parse_releases_exact_working_set() {
    const std::filesystem::path root = unique_root("html-resource-success");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    constexpr std::string_view html =
        "<main><p id=\"alpha\" role=\"note\">hello</p></main>";

    std::string error;
    if (!require(build_store(store_root, html, 3U, &error), error)) {
        return false;
    }

    StreamingHtmlNodeSourceConfig config;
    config.input_window_bytes = 7U;
    config.working_set_limit_bytes = 1024U * 1024U;
    StreamingHtmlNodeSourceStats stats;
    if (!require(
            produce_streaming_html_node_source(
                store_root, source_path, config, &stats, &error),
            error) ||
        !require(std::filesystem::exists(source_path), "source sidecar published") ||
        !require(
            stats.working_set_hard_limit_bytes == config.working_set_limit_bytes,
            "hard limit reported exactly") ||
        !require(stats.working_set_peak_bytes > 0U, "real parser allocations are charged") ||
        !require(
            stats.working_set_peak_bytes <= config.working_set_limit_bytes,
            "peak remains within hard cap") ||
        !require(stats.working_set_current_bytes == 0U, "all parser allocations released") ||
        !require(stats.working_set_reservations > 0U, "reservations are visible") ||
        !require(
            stats.working_set_reservations == stats.working_set_releases,
            "allocation/deallocation reservation counts match") ||
        !require(
            stats.working_set_rejected_reservations == 0U,
            "successful parse has no rejected reservation") ||
        !require(
            stats.working_set_accounting_errors == 0U,
            "successful parse has clean accounting")) {
        return false;
    }
    return true;
}

bool test_hard_cap_rejection_fails_closed() {
    const std::filesystem::path root = unique_root("html-resource-reject");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    constexpr std::string_view html =
        "<main><p id=\"abcdefghijklmnopqrstuvwxyz\">hello</p></main>";

    std::string error;
    if (!require(build_store(store_root, html, 3U, &error), error)) {
        return false;
    }

    StreamingHtmlNodeSourceConfig config;
    config.working_set_limit_bytes = 1U;
    StreamingHtmlNodeSourceStats stats;
    if (!require(
            !produce_streaming_html_node_source(
                store_root, source_path, config, &stats, &error),
            "tiny parser working-set cap is rejected") ||
        !require(
            error.find("working-set hard limit exhausted") != std::string::npos,
            "hard-cap failure is classified") ||
        !require(!std::filesystem::exists(source_path), "failed parse cannot publish source") ||
        !require(
            !std::filesystem::exists(
                std::filesystem::path(source_path.string() + ".building")),
            "failed parse removes building sidecar") ||
        !require(
            stats.working_set_rejected_reservations > 0U,
            "rejected reservation is visible") ||
        !require(stats.working_set_current_bytes == 0U, "rejection leaks no reservation") ||
        !require(
            stats.working_set_accounting_errors == 0U,
            "rejection preserves accounting integrity")) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!test_successful_parse_releases_exact_working_set() ||
        !test_hard_cap_rejection_fails_closed()) {
        return 1;
    }
    std::cout << "Streaming HTML resource accounting tests passed\n";
    return 0;
}
