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

constexpr std::uint64_t kRootSeed = 0x5a375f48544d4c32ULL;
constexpr std::size_t kCaseCount = 64U;
constexpr std::array<std::size_t, 7> kWindows{{1U, 2U, 3U, 5U, 7U, 16U, 64U}};

struct GeneratedCase {
    std::string html;
    std::uint64_t logical_nodes{0U};
    bool should_succeed{false};
    std::string expected_error_fragment;
};

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

std::uint64_t splitmix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::uint64_t case_seed(std::size_t index) noexcept {
    return splitmix64(kRootSeed ^ static_cast<std::uint64_t>(index));
}

bool fail_case(
    std::size_t index,
    std::uint64_t seed,
    std::string_view message) {
    std::cerr << "FAILED: Z7 parser property fuzz case=" << index
              << " seed=" << seed << ": " << message << '\n';
    return false;
}

std::string safe_text(std::mt19937_64* random, std::size_t minimum, std::size_t maximum) {
    constexpr std::string_view alphabet =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 &;{}:._-";
    const std::size_t length = minimum +
        static_cast<std::size_t>((*random)() % (maximum - minimum + 1U));
    std::string result;
    result.reserve(length);
    for (std::size_t index = 0U; index < length; ++index) {
        result.push_back(alphabet[static_cast<std::size_t>((*random)() % alphabet.size())]);
    }
    return result;
}

GeneratedCase generate_case(std::size_t index, std::uint64_t seed) {
    std::mt19937_64 random(seed);
    switch (index % 8U) {
    case 0U: {
        const std::string text = safe_text(&random, 1U, 24U);
        return GeneratedCase{
            "<div data-x='a&amp;b'>" + text + "</div>", 3U, true, {}};
    }
    case 1U: {
        const std::string left = safe_text(&random, 1U, 10U);
        const std::string middle = safe_text(&random, 1U, 10U);
        const std::string right = safe_text(&random, 1U, 10U);
        return GeneratedCase{
            "<div>" + left + "<span role='note'>" + middle +
                "</span>" + right + "</div>",
            6U,
            true,
            {}};
    }
    case 2U: {
        const std::string payload =
            "a<" + safe_text(&random, 1U, 12U) + "</stylex><b{c:d}";
        return GeneratedCase{
            "<style>" + payload + "</STYLE \t>", 3U, true, {}};
    }
    case 3U: {
        constexpr std::array<std::string_view, 4> tags{{
            "xmp", "iframe", "noembed", "noframes"}};
        const std::string tag(tags[static_cast<std::size_t>(random() % tags.size())]);
        const std::string payload =
            "q</" + tag + "x><" + safe_text(&random, 1U, 12U) + "{z:1}";
        return GeneratedCase{
            "<" + tag + ">" + payload + "</" + tag + " >", 3U, true, {}};
    }
    case 4U: {
        const std::string payload =
            "a&amp;<b</titlex>" + safe_text(&random, 1U, 12U);
        return GeneratedCase{
            "<title>" + payload + "</TITLE>", 3U, true, {}};
    }
    case 5U: {
        const std::string payload =
            "a&amp;<b</textareax>" + safe_text(&random, 1U, 12U);
        return GeneratedCase{
            "<textarea>\n" + payload + "</textarea>", 3U, true, {}};
    }
    case 6U:
        return GeneratedCase{
            "<script>x</script>",
            3U,
            false,
            "special HTML tokenizer state is not implemented"};
    default:
        if ((random() & 1U) == 0U) {
            return GeneratedCase{
                "<div/>",
                1U,
                false,
                "self-closing syntax on non-void"};
        }
        return GeneratedCase{
            "<div><span>x</div></span>",
            4U,
            false,
            "mismatched HTML end tag"};
    }
}

std::vector<std::string> split_records(std::string_view html, std::uint64_t seed) {
    std::mt19937_64 random(seed);
    std::vector<std::string> records;
    std::size_t offset = 0U;
    while (offset < html.size()) {
        const std::size_t remaining = html.size() - offset;
        const std::size_t maximum = std::min<std::size_t>(7U, remaining);
        const std::size_t count = 1U +
            static_cast<std::size_t>(random() % maximum);
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
        const std::string_view record = records[index];
        if (!writer.append(
                9601U + static_cast<std::uint64_t>(index),
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
    metadata.logical_records = static_cast<std::uint64_t>(records.size());
    metadata.logical_nodes = generated.logical_nodes;
    metadata.largest_record_bytes = largest;
    return writer.finalize(metadata, nullptr, error);
}

bool run_success_case(
    std::size_t case_index,
    std::uint64_t seed,
    const GeneratedCase& generated,
    const std::filesystem::path& case_root) {
    const std::filesystem::path store_root = case_root / "store";
    std::string error;
    if (!build_store(store_root, generated, splitmix64(seed), &error)) {
        return fail_case(case_index, seed, error);
    }

    std::vector<std::byte> reference;
    for (const std::size_t window : kWindows) {
        const std::filesystem::path source_path =
            case_root / ("nodes-" + std::to_string(window) + ".zvnsrc");
        StreamingHtmlNodeSourceConfig config;
        config.input_window_bytes = window;
        StreamingHtmlNodeSourceV2Stats stats;
        error.clear();
        if (!produce_streaming_html_node_source_v2(
                store_root, source_path, config, &stats, &error)) {
            return fail_case(
                case_index,
                seed,
                "valid case failed at window " + std::to_string(window) + ": " + error);
        }
        if (stats.working_set_current_bytes != 0U ||
            stats.working_set_accounting_errors != 0U ||
            stats.working_set_peak_bytes > stats.working_set_hard_limit_bytes) {
            return fail_case(case_index, seed, "working-set authority disagrees after success");
        }

        LogicalNodeSourceV2ValidationStats validation;
        if (!validate_logical_node_source_v2_against_store(
                source_path, store_root, &validation, &error)) {
            return fail_case(case_index, seed, "source validation failed: " + error);
        }
        if (validation.nodes_validated != generated.logical_nodes) {
            return fail_case(case_index, seed, "validated node count disagrees with generated oracle");
        }

        const std::vector<std::byte> output = read_file(source_path);
        if (output.empty()) {
            return fail_case(case_index, seed, "successful source output is unreadable");
        }
        if (reference.empty()) {
            reference = output;
        } else if (output != reference) {
            return fail_case(
                case_index,
                seed,
                "ZVNSRC v2 output changes with StoreReader input-window size");
        }
    }
    return true;
}

bool run_failure_case(
    std::size_t case_index,
    std::uint64_t seed,
    const GeneratedCase& generated,
    const std::filesystem::path& case_root) {
    const std::filesystem::path store_root = case_root / "store";
    std::string error;
    if (!build_store(store_root, generated, splitmix64(seed), &error)) {
        return fail_case(case_index, seed, error);
    }

    std::string reference_error;
    for (const std::size_t window : kWindows) {
        const std::filesystem::path source_path =
            case_root / ("rejected-" + std::to_string(window) + ".zvnsrc");
        StreamingHtmlNodeSourceConfig config;
        config.input_window_bytes = window;
        StreamingHtmlNodeSourceV2Stats stats;
        error.clear();
        if (produce_streaming_html_node_source_v2(
                store_root, source_path, config, &stats, &error)) {
            return fail_case(case_index, seed, "invalid case unexpectedly parsed successfully");
        }
        if (error.empty() ||
            error.find(generated.expected_error_fragment) == std::string::npos) {
            return fail_case(case_index, seed, "invalid case returned unexpected failure class: " + error);
        }
        if (reference_error.empty()) {
            reference_error = error;
        } else if (error != reference_error) {
            return fail_case(
                case_index,
                seed,
                "fail-closed error changes with StoreReader input-window size");
        }
        if (stats.working_set_current_bytes != 0U ||
            stats.working_set_accounting_errors != 0U) {
            return fail_case(case_index, seed, "working-set authority disagrees after rejection");
        }
        if (std::filesystem::exists(source_path) ||
            std::filesystem::exists(
                std::filesystem::path(source_path.string() + ".building"))) {
            return fail_case(case_index, seed, "rejected parse published source or staging output");
        }
    }
    return true;
}

} // namespace

int main() {
    const std::filesystem::path root = unique_root("html-v2-property-fuzz");
    RootCleanup cleanup(root);
    std::error_code filesystem_error;
    std::filesystem::create_directories(root, filesystem_error);
    if (filesystem_error) {
        std::cerr << "FAILED: cannot create Z7 property-fuzz work root: "
                  << filesystem_error.message() << '\n';
        return 1;
    }

    for (std::size_t index = 0U; index < kCaseCount; ++index) {
        const std::uint64_t seed = case_seed(index);
        const GeneratedCase generated = generate_case(index, seed);
        const std::filesystem::path case_root = root / ("case-" + std::to_string(index));
        filesystem_error.clear();
        std::filesystem::create_directories(case_root, filesystem_error);
        if (filesystem_error) {
            fail_case(index, seed, "cannot create case work directory");
            return 1;
        }
        const bool passed = generated.should_succeed
            ? run_success_case(index, seed, generated, case_root)
            : run_failure_case(index, seed, generated, case_root);
        if (!passed) {
            return 1;
        }
    }

    std::cout << "Z7 parser property fuzz smoke PASS seed=" << kRootSeed
              << " cases=" << kCaseCount
              << " windows=" << kWindows.size() << '\n';
    return 0;
}
