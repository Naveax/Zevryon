#include "logical_node_source_v2.hpp"
#include "massivedoc_store.hpp"
#include "streaming_html_node_source_v2.hpp"

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
using zevryon::massivedoc::LogicalNodeSourceV2ValidationStats;
using zevryon::massivedoc::StoreWriter;
using zevryon::massivedoc::StreamingHtmlNodeSourceConfig;
using zevryon::massivedoc::StreamingHtmlNodeSourceV2Stats;
using zevryon::massivedoc::produce_streaming_html_node_source_v2;
using zevryon::massivedoc::validate_logical_node_source_v2_against_store;

constexpr std::array<std::size_t, 16> kWindows{{
    1U, 2U, 3U, 4U, 5U, 7U, 8U, 15U,
    16U, 31U, 32U, 63U, 64U, 127U, 255U, 4096U}};

struct Case {
    std::string_view name;
    std::string_view html;
    std::uint64_t logical_nodes;
    bool should_succeed;
    std::string_view error_fragment;
};

constexpr std::array<Case, 5> kCases{{
    {"data-text-and-attributes",
     "<div data-x='a&amp;b'>alpha<span role='note'>beta</span>gamma</div>",
     6U,
     true,
     {}},
    {"rawtext-style",
     "<style>a<q>b</q>&amp;</style>",
     3U,
     true,
     {}},
    {"rcdata-title",
     "<title>a&amp;b</title>",
     3U,
     true,
     {}},
    {"plaintext-eof",
     "<plaintext>a<b>&amp;",
     3U,
     true,
     {}},
    {"mismatched-end-tag",
     "<div><span>x</div></span>",
     4U,
     false,
     "mismatched HTML end tag"},
}};

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: Z7 chunk-size equivalence: " << message << '\n';
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

bool build_store(
    const std::filesystem::path& root,
    const Case& test_case,
    std::string* error) {
    StoreWriter writer(root);
    if (!writer.append(1U, bytes(test_case.html), error)) {
        return false;
    }
    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = static_cast<std::uint64_t>(test_case.html.size());
    metadata.logical_records = 1U;
    metadata.logical_nodes = test_case.logical_nodes;
    metadata.largest_record_bytes = static_cast<std::uint64_t>(test_case.html.size());
    return writer.finalize(metadata, nullptr, error);
}

bool run_success_case(
    const Case& test_case,
    const std::filesystem::path& root) {
    const auto store_root = root / "store";
    std::string error;
    if (!require(build_store(store_root, test_case, &error), error)) {
        return false;
    }

    std::vector<std::byte> reference;
    StreamingHtmlNodeSourceV2Stats reference_stats{};
    bool have_reference = false;
    for (const std::size_t window : kWindows) {
        const auto source_path = root / ("nodes-" + std::to_string(window) + ".zvnsrc");
        StreamingHtmlNodeSourceConfig config;
        config.input_window_bytes = window;
        StreamingHtmlNodeSourceV2Stats stats;
        error.clear();
        if (!require(
                produce_streaming_html_node_source_v2(
                    store_root, source_path, config, &stats, &error),
                std::string(test_case.name) + " window=" + std::to_string(window) + ": " + error)) {
            return false;
        }
        if (!require(stats.nodes_emitted == test_case.logical_nodes,
                     "successful node count drifted with input window") ||
            !require(stats.working_set_current_bytes == 0U,
                     "successful parse leaked charged working set") ||
            !require(stats.working_set_accounting_errors == 0U,
                     "successful parse reported accounting error")) {
            return false;
        }

        LogicalNodeSourceV2ValidationStats validation;
        if (!require(
                validate_logical_node_source_v2_against_store(
                    source_path, store_root, &validation, &error),
                error) ||
            !require(validation.nodes_validated == test_case.logical_nodes,
                     "validated node count drifted")) {
            return false;
        }

        const std::vector<std::byte> output = read_file(source_path);
        if (!require(!output.empty(), "successful parse produced unreadable source")) {
            return false;
        }
        if (!have_reference) {
            reference = output;
            reference_stats = stats;
            have_reference = true;
        } else {
            if (!require(output == reference,
                         "ZVNSRC bytes changed with StoreReader input-window size") ||
                !require(stats.source_records == reference_stats.source_records &&
                             stats.source_bytes == reference_stats.source_bytes &&
                             stats.nodes_emitted == reference_stats.nodes_emitted &&
                             stats.element_nodes_emitted == reference_stats.element_nodes_emitted &&
                             stats.text_nodes_emitted == reference_stats.text_nodes_emitted &&
                             stats.attributes_emitted == reference_stats.attributes_emitted &&
                             stats.comments_skipped == reference_stats.comments_skipped &&
                             stats.doctypes_skipped == reference_stats.doctypes_skipped &&
                             stats.cross_record_markup_spans == reference_stats.cross_record_markup_spans &&
                             stats.cross_record_text_spans == reference_stats.cross_record_text_spans &&
                             stats.maximum_observed_open_depth == reference_stats.maximum_observed_open_depth,
                         "semantic parser stats changed with input-window size")) {
                return false;
            }
        }
    }
    return true;
}

bool run_failure_case(
    const Case& test_case,
    const std::filesystem::path& root) {
    const auto store_root = root / "store";
    std::string error;
    if (!require(build_store(store_root, test_case, &error), error)) {
        return false;
    }

    std::string reference_error;
    for (const std::size_t window : kWindows) {
        const auto source_path = root / ("rejected-" + std::to_string(window) + ".zvnsrc");
        StreamingHtmlNodeSourceConfig config;
        config.input_window_bytes = window;
        StreamingHtmlNodeSourceV2Stats stats;
        error.clear();
        if (!require(
                !produce_streaming_html_node_source_v2(
                    store_root, source_path, config, &stats, &error),
                "invalid input unexpectedly parsed successfully") ||
            !require(error.find(test_case.error_fragment) != std::string::npos,
                     "invalid input changed failure class") ||
            !require(stats.working_set_current_bytes == 0U,
                     "rejected parse leaked charged working set") ||
            !require(stats.working_set_accounting_errors == 0U,
                     "rejected parse reported accounting error") ||
            !require(!std::filesystem::exists(source_path),
                     "rejected parse published final source") ||
            !require(!std::filesystem::exists(
                         std::filesystem::path(source_path.string() + ".building")),
                     "rejected parse left staging source")) {
            return false;
        }
        if (reference_error.empty()) {
            reference_error = error;
        } else if (!require(error == reference_error,
                            "fail-closed error changed with input-window size")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    const auto root = unique_root("html-v2-chunk-equivalence");
    RootCleanup cleanup(root);
    std::error_code filesystem_error;
    std::filesystem::create_directories(root, filesystem_error);
    if (!require(!filesystem_error, "cannot create work root")) {
        return 1;
    }

    for (std::size_t index = 0U; index < kCases.size(); ++index) {
        const Case& test_case = kCases[index];
        const auto case_root = root / ("case-" + std::to_string(index));
        filesystem_error.clear();
        std::filesystem::create_directories(case_root, filesystem_error);
        if (!require(!filesystem_error, "cannot create case root")) {
            return 1;
        }
        const bool ok = test_case.should_succeed
            ? run_success_case(test_case, case_root)
            : run_failure_case(test_case, case_root);
        if (!ok) {
            return 1;
        }
    }

    std::cout << "Z7 parser chunk-size equivalence PASS cases=" << kCases.size()
              << " windows=" << kWindows.size() << '\n';
    return 0;
}
