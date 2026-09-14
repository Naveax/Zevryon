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
#include <random>
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

constexpr std::uint64_t kRootSeed = 0x7a375f66757a7a31ULL;
constexpr std::size_t kCaseCount = 10'000U;
constexpr std::array<std::size_t, 7> kWindows{{1U, 2U, 3U, 7U, 16U, 64U, 4096U}};

struct GeneratedCase {
    std::string html;
    std::uint64_t logical_nodes{0U};
    bool should_succeed{false};
    std::string expected_error_fragment;
};

bool require(bool condition, std::size_t index, std::uint64_t seed, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: Z7 parser fuzz certification case=" << index
                  << " seed=" << seed << ": " << message << '\n';
        return false;
    }
    return true;
}

std::uint64_t splitmix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::uint64_t case_seed(std::size_t index) noexcept {
    return splitmix64(kRootSeed ^ static_cast<std::uint64_t>(index));
}

std::string text_payload(std::mt19937_64* random, std::size_t minimum, std::size_t maximum) {
    constexpr std::string_view alphabet =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-{}:.;";
    const std::size_t length = minimum +
        static_cast<std::size_t>((*random)() % (maximum - minimum + 1U));
    std::string result;
    result.reserve(length);
    for (std::size_t i = 0U; i < length; ++i) {
        result.push_back(alphabet[static_cast<std::size_t>((*random)() % alphabet.size())]);
    }
    return result;
}

GeneratedCase generate_case(std::size_t index, std::uint64_t seed) {
    std::mt19937_64 random(seed);
    const std::string a = text_payload(&random, 1U, 24U);
    const std::string b = text_payload(&random, 1U, 24U);
    switch (index % 12U) {
    case 0U:
        return {"<div>" + a + "</div>", 3U, true, {}};
    case 1U:
        return {"<div><span>" + a + "</span>" + b + "</div>", 5U, true, {}};
    case 2U:
        return {"<style>" + a + "<q>" + b + "</q>&amp;</style>", 3U, true, {}};
    case 3U:
        return {"<title>" + a + "&amp;" + b + "</title>", 3U, true, {}};
    case 4U:
        return {"<textarea>\n" + a + "&amp;" + b + "</textarea>", 3U, true, {}};
    case 5U:
        return {"<plaintext>" + a + "<b>" + b + "&amp;", 3U, true, {}};
    case 6U:
        return {"<p data-x='a&amp;b' role='note'>" + a + "</p>", 3U, true, {}};
    case 7U:
        return {"<div>a<br>b</div>", 5U, true, {}};
    case 8U:
        return {"<script>x</script>", 3U, false, "special HTML tokenizer state is not implemented"};
    case 9U:
        return {"<div><span>x</div></span>", 4U, false, "mismatched HTML end tag"};
    case 10U:
        return {"<div/>", 1U, false, "self-closing syntax on non-void"};
    default:
        return {"<svg></svg>", 1U, false, "foreign-content HTML element is not implemented"};
    }
}

std::filesystem::path unique_root() {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (std::string("zevryon-z7-parser-fuzz-cert-") + std::to_string(tick));
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
        reinterpret_cast<const std::byte*>(value.data()), value.size());
}

std::vector<std::string> split_records(std::string_view html, std::uint64_t seed) {
    std::mt19937_64 random(seed);
    std::vector<std::string> records;
    std::size_t offset = 0U;
    while (offset < html.size()) {
        const std::size_t remaining = html.size() - offset;
        const std::size_t maximum = std::min<std::size_t>(17U, remaining);
        const std::size_t count = 1U + static_cast<std::size_t>(random() % maximum);
        records.emplace_back(html.substr(offset, count));
        offset += count;
    }
    return records;
}

bool build_store(
    const std::filesystem::path& store_root,
    const GeneratedCase& generated,
    std::uint64_t seed,
    std::string* error) {
    const std::vector<std::string> records = split_records(generated.html, seed);
    StoreWriter writer(store_root);
    std::uint64_t total = 0U;
    std::uint64_t largest = 0U;
    for (std::size_t index = 0U; index < records.size(); ++index) {
        if (!writer.append(
                10'001U + static_cast<std::uint64_t>(index),
                bytes(records[index]),
                error)) {
            return false;
        }
        const auto size = static_cast<std::uint64_t>(records[index].size());
        total += size;
        largest = std::max(largest, size);
    }
    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = total;
    metadata.logical_records = static_cast<std::uint64_t>(records.size());
    metadata.logical_nodes = generated.logical_nodes;
    metadata.largest_record_bytes = largest;
    return writer.finalize(metadata, nullptr, error);
}

bool execute_case(
    std::size_t index,
    std::uint64_t seed,
    const GeneratedCase& generated,
    const std::filesystem::path& root) {
    const auto case_root = root / ("case-" + std::to_string(index));
    const auto store_root = case_root / "store";
    const auto source_path = case_root / "nodes.zvnsrc";
    std::error_code filesystem_error;
    std::filesystem::create_directories(case_root, filesystem_error);
    if (!require(!filesystem_error, index, seed, "cannot create case root")) {
        return false;
    }

    std::string error;
    if (!require(
            build_store(store_root, generated, splitmix64(seed), &error),
            index,
            seed,
            error)) {
        return false;
    }

    StreamingHtmlNodeSourceConfig config;
    config.input_window_bytes = kWindows[static_cast<std::size_t>(seed % kWindows.size())];
    config.working_set_limit_bytes = 2U * 1024U * 1024U;
    StreamingHtmlNodeSourceV2Stats stats;
    const bool success = produce_streaming_html_node_source_v2(
        store_root, source_path, config, &stats, &error);

    if (generated.should_succeed) {
        if (!require(success, index, seed, error) ||
            !require(stats.nodes_emitted == generated.logical_nodes,
                     index, seed, "successful node count disagrees with oracle") ||
            !require(stats.working_set_current_bytes == 0U,
                     index, seed, "successful parse leaked charged working set") ||
            !require(stats.working_set_accounting_errors == 0U,
                     index, seed, "successful parse reported accounting error") ||
            !require(stats.working_set_peak_bytes <= stats.working_set_hard_limit_bytes,
                     index, seed, "successful parse exceeded hard working-set limit")) {
            return false;
        }
        LogicalNodeSourceV2ValidationStats validation;
        if (!require(
                validate_logical_node_source_v2_against_store(
                    source_path, store_root, &validation, &error),
                index,
                seed,
                error) ||
            !require(validation.nodes_validated == generated.logical_nodes,
                     index, seed, "validator node count disagrees with oracle")) {
            return false;
        }
    } else {
        if (!require(!success, index, seed, "invalid case unexpectedly succeeded") ||
            !require(error.find(generated.expected_error_fragment) != std::string::npos,
                     index, seed, "invalid case returned unexpected failure class") ||
            !require(stats.working_set_current_bytes == 0U,
                     index, seed, "rejected parse leaked charged working set") ||
            !require(stats.working_set_accounting_errors == 0U,
                     index, seed, "rejected parse reported accounting error") ||
            !require(!std::filesystem::exists(source_path),
                     index, seed, "rejected parse published final source") ||
            !require(!std::filesystem::exists(
                         std::filesystem::path(source_path.string() + ".building")),
                     index, seed, "rejected parse left staging source")) {
            return false;
        }
    }

    filesystem_error.clear();
    std::filesystem::remove_all(case_root, filesystem_error);
    return require(!filesystem_error, index, seed, "cannot retire completed case root");
}

} // namespace

int main() {
    const auto root = unique_root();
    RootCleanup cleanup(root);
    std::error_code filesystem_error;
    std::filesystem::create_directories(root, filesystem_error);
    if (filesystem_error) {
        std::cerr << "FAILED: cannot create Z7 parser fuzz certification root\n";
        return 1;
    }

    const auto started = std::chrono::steady_clock::now();
    std::size_t successful_oracles = 0U;
    std::size_t rejected_oracles = 0U;
    for (std::size_t index = 0U; index < kCaseCount; ++index) {
        const std::uint64_t seed = case_seed(index);
        const GeneratedCase generated = generate_case(index, seed);
        if (generated.should_succeed) {
            ++successful_oracles;
        } else {
            ++rejected_oracles;
        }
        if (!execute_case(index, seed, generated, root)) {
            return 1;
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    std::cout << "Z7 parser fuzz certification PASS seed=" << kRootSeed
              << " cases=" << kCaseCount
              << " success_oracles=" << successful_oracles
              << " rejection_oracles=" << rejected_oracles
              << " windows=" << kWindows.size()
              << " elapsed_ms=" << elapsed.count() << '\n';
    return 0;
}
