#include "massivedoc_store.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace zevryon::massivedoc;

#define REQUIRE(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "REQUIRE failed: " #condition " at " << __FILE__ << ':' << __LINE__ << "\n"; \
            std::exit(1); \
        } \
    } while (false)

std::vector<std::byte> bytes_of(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(begin, begin + text.size());
}

std::filesystem::path test_root(std::string_view name) {
    const auto root = std::filesystem::temp_directory_path() /
        ("zevryon-m4-unicode-search-" + std::string(name));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    REQUIRE(!ignored);
    return root;
}

void append_text(StoreWriter* writer, std::uint64_t id, std::string_view text) {
    std::string error;
    const auto bytes = bytes_of(text);
    REQUIRE(writer->append(id, bytes, &error));
}

void build_valid_store(const std::filesystem::path& root) {
    StoreConfig config;
    config.records_per_search_block = 2U;
    StoreWriter writer(root, config);
    append_text(&writer, 100U, "xxStra\xC3\x9F" "e yy");
    append_text(&writer, 101U, "A\xCC\x8Aland");
    append_text(&writer, 102U, "plain unrelated record");
    StoreStats stats;
    std::string error;
    REQUIRE(writer.finalize({}, &stats, &error));
    REQUIRE(stats.corpus.logical_records == 3U);
}

void test_casefold_canonical_and_no_raw_index_dependency() {
    const auto root = test_root("equivalence");
    build_valid_store(root);

    StoreReadConfig read_config;
    read_config.io_window_bytes = 7U; // splits the two-byte sharp-s across decoder feeds.
    StoreReader reader(root, read_config);
    std::string error;
    REQUIRE(reader.open(&error));

    // Normalized search deliberately does not use the raw bigram/trigram candidate layer.
    // Removing search.bgm after open makes any accidental legacy-index dependency fail.
    std::error_code fs_error;
    REQUIRE(std::filesystem::remove(root / "search.bgm", fs_error));
    REQUIRE(!fs_error);

    UnicodeSearchExecutionStats execution;
    const auto fold_hits = reader.find_unicode_bounded(
        "STRASSE",
        10U,
        [] { return false; },
        {},
        &execution,
        &error);
    REQUIRE(error.empty());
    REQUIRE(fold_hits.size() == 1U);
    REQUIRE(fold_hits[0].record_index == 0U);
    REQUIRE(fold_hits[0].logical_id == 100U);
    REQUIRE(fold_hits[0].byte_offset == 2U);
    REQUIRE(execution.records_scanned == 3U);
    REQUIRE(execution.query_normalized_codepoints == 7U);
    REQUIRE(execution.source_bytes_decoded != 0U);
    REQUIRE(execution.normalized_codepoints != 0U);
    REQUIRE(!execution.cancelled);

    const auto canonical_hits = reader.find_unicode_bounded(
        "\xC3\x85LAND",
        10U,
        [] { return false; },
        {},
        &execution,
        &error);
    REQUIRE(error.empty());
    REQUIRE(canonical_hits.size() == 1U);
    REQUIRE(canonical_hits[0].record_index == 1U);
    REQUIRE(canonical_hits[0].logical_id == 101U);
    REQUIRE(canonical_hits[0].byte_offset == 0U);

    std::filesystem::remove_all(root, fs_error);
    REQUIRE(!fs_error);
}

void test_cancellation_discards_prior_hits() {
    const auto root = test_root("cancel");
    build_valid_store(root);
    StoreReader reader(root);
    std::string error;
    REQUIRE(reader.open(&error));

    std::uint64_t checks = 0U;
    UnicodeSearchExecutionStats execution;
    const auto hits = reader.find_unicode_bounded(
        "STRASSE",
        10U,
        [&checks] {
            ++checks;
            return checks >= 3U;
        },
        {},
        &execution,
        &error);
    REQUIRE(hits.empty());
    REQUIRE(error == "search cancelled");
    REQUIRE(execution.cancelled);
    REQUIRE(execution.records_scanned == 1U);

    std::error_code fs_error;
    std::filesystem::remove_all(root, fs_error);
    REQUIRE(!fs_error);
}

void test_invalid_utf8_fails_closed() {
    const auto root = test_root("invalid");
    StoreWriter writer(root, {});
    std::string error;
    const std::array<std::byte, 3> invalid{
        std::byte{static_cast<unsigned char>('x')}, std::byte{0xff}, std::byte{static_cast<unsigned char>('y')}};
    REQUIRE(writer.append(200U, invalid, &error));
    StoreStats stats;
    REQUIRE(writer.finalize({}, &stats, &error));

    StoreReader reader(root);
    REQUIRE(reader.open(&error));
    UnicodeSearchExecutionStats execution;
    const auto hits = reader.find_unicode_bounded(
        "abc",
        10U,
        [] { return false; },
        {},
        &execution,
        &error);
    REQUIRE(hits.empty());
    REQUIRE(error.find("invalid UTF-8") != std::string::npos);
    REQUIRE(!execution.cancelled);

    std::error_code fs_error;
    std::filesystem::remove_all(root, fs_error);
    REQUIRE(!fs_error);
}

void test_query_and_pending_bounds_fail_closed() {
    const auto root = test_root("bounds");
    StoreWriter writer(root, {});
    append_text(&writer, 300U, "a\xCC\x8A\xCC\x95");
    StoreStats stats;
    std::string error;
    REQUIRE(writer.finalize({}, &stats, &error));
    StoreReader reader(root);
    REQUIRE(reader.open(&error));

    UnicodeSearchOptions query_options;
    query_options.max_query_codepoints = 1U;
    UnicodeSearchExecutionStats execution;
    const auto query_hits = reader.find_unicode_bounded(
        "\xC3\x9F",
        10U,
        [] { return false; },
        query_options,
        &execution,
        &error);
    REQUIRE(query_hits.empty());
    REQUIRE(error.find("codepoint bound") != std::string::npos);

    UnicodeSearchOptions pending_options;
    pending_options.max_pending_codepoints = 2U;
    const auto pending_hits = reader.find_unicode_bounded(
        "x",
        10U,
        [] { return false; },
        pending_options,
        &execution,
        &error);
    REQUIRE(pending_hits.empty());
    REQUIRE(error.find("pending sequence") != std::string::npos);

    std::error_code fs_error;
    std::filesystem::remove_all(root, fs_error);
    REQUIRE(!fs_error);
}

void test_invalid_query_utf8_is_explicit_error() {
    const auto root = test_root("bad-query");
    build_valid_store(root);
    StoreReader reader(root);
    std::string error;
    REQUIRE(reader.open(&error));
    const std::string invalid_query("\xC3", 1);
    UnicodeSearchExecutionStats execution;
    const auto hits = reader.find_unicode_bounded(
        invalid_query,
        10U,
        [] { return false; },
        {},
        &execution,
        &error);
    REQUIRE(hits.empty());
    REQUIRE(error.find("invalid UTF-8") != std::string::npos);
    REQUIRE(execution.records_scanned == 0U);

    std::error_code fs_error;
    std::filesystem::remove_all(root, fs_error);
    REQUIRE(!fs_error);
}

} // namespace

int main() {
    test_casefold_canonical_and_no_raw_index_dependency();
    test_cancellation_discards_prior_hits();
    test_invalid_utf8_fails_closed();
    test_query_and_pending_bounds_fail_closed();
    test_invalid_query_utf8_is_explicit_error();
    std::cout << "Zevryon MassiveDoc Unicode search integration tests passed\n";
    return 0;
}
