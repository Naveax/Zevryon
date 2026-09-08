#include "logical_node_arena.hpp"
#include "logical_node_arena_v2_store_bound.hpp"
#include "logical_node_record_index.hpp"
#include "logical_node_source.hpp"
#include "massivedoc_store.hpp"

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

namespace {

using zevryon::massivedoc::CorpusMetadata;
using zevryon::massivedoc::LogicalNodeArenaBuildConfig;
using zevryon::massivedoc::LogicalNodeArenaV2StoreBoundWriter;
using zevryon::massivedoc::LogicalNodeInput;
using zevryon::massivedoc::LogicalNodeRecordIndexReader;
using zevryon::massivedoc::LogicalNodeRecordIndexWindow;
using zevryon::massivedoc::LogicalNodeSourceStoreBinding;
using zevryon::massivedoc::StoreWriter;
using zevryon::massivedoc::build_logical_node_record_index;
using zevryon::massivedoc::inspect_logical_node_source_store_binding;
using zevryon::massivedoc::kNoLogicalNodeOrdinal;
using zevryon::massivedoc::kNoLogicalNodeRecordPosting;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: logical node record-index corruption: "
                  << message << '\n';
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

std::span<const std::byte> bytes(std::string_view text) {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size());
}

bool build_store(
    const std::filesystem::path& root,
    bool alternate_partition,
    std::string* error) {
    constexpr std::array<std::string_view, 3> partition_a{{"ab", "cdef", "gh"}};
    constexpr std::array<std::string_view, 2> partition_b{{"abc", "defgh"}};

    StoreWriter writer(root);
    std::uint64_t largest = 0U;
    if (alternate_partition) {
        for (std::size_t index = 0U; index < partition_b.size(); ++index) {
            if (!writer.append(
                    9701U + static_cast<std::uint64_t>(index),
                    bytes(partition_b[index]),
                    error)) {
                return false;
            }
            largest = std::max<std::uint64_t>(largest, partition_b[index].size());
        }
    } else {
        for (std::size_t index = 0U; index < partition_a.size(); ++index) {
            if (!writer.append(
                    9701U + static_cast<std::uint64_t>(index),
                    bytes(partition_a[index]),
                    error)) {
                return false;
            }
            largest = std::max<std::uint64_t>(largest, partition_a[index].size());
        }
    }

    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = 8U;
    metadata.logical_records = alternate_partition ? 2U : 3U;
    metadata.logical_nodes = 4U;
    metadata.largest_record_bytes = largest;
    return writer.finalize(metadata, nullptr, error);
}

bool build_arena(const std::filesystem::path& root, std::string* error) {
    LogicalNodeSourceStoreBinding binding;
    if (!inspect_logical_node_source_store_binding(root, &binding, error)) {
        return false;
    }
    LogicalNodeArenaBuildConfig config;
    config.candidate_commit =
        "0123456789abcdef0123456789abcdef01234567";
    config.candidate_tree =
        "89abcdef0123456789abcdef0123456789abcdef";
    config.source_sha256 = binding.payload_sha256;
    config.semantic_bucket_count = 64U;
    config.semantic_hash_bits = 64U;

    LogicalNodeArenaV2StoreBoundWriter writer(root, config);
    return writer.begin(error) &&
        writer.append_node(
            LogicalNodeInput{
                1U, 0U, 0U, 0U, kNoLogicalNodeOrdinal,
                "#document", "", "", 0U},
            {},
            error) &&
        writer.append_node(
            LogicalNodeInput{
                2U, 0U, 0U, 1U, 0U,
                "div", "", "", 0U},
            {},
            error) &&
        writer.append_node(
            LogicalNodeInput{
                3U, 0U, 1U, 6U, 1U,
                "#text", "", "", 0U},
            {},
            error) &&
        writer.append_node(
            LogicalNodeInput{
                4U, 1U, 1U, 2U, 1U,
                "span", "note", "display:inline", 0U},
            {},
            error) &&
        writer.finish(error);
}

bool rebuild_index(const std::filesystem::path& root, std::string* error) {
    std::error_code remove_error;
    std::filesystem::remove_all(root / "node-record-index-v1", remove_error);
    if (remove_error) {
        *error = "cannot remove record-index fixture: " + remove_error.message();
        return false;
    }
    return build_logical_node_record_index(root, error);
}

bool flip_byte(const std::filesystem::path& path, std::uint64_t offset) {
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) {
        return false;
    }
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    char value = 0;
    stream.read(&value, 1);
    if (!stream) {
        return false;
    }
    value = static_cast<char>(static_cast<unsigned char>(value) ^ 0x01U);
    stream.clear();
    stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    stream.write(&value, 1);
    stream.flush();
    return static_cast<bool>(stream);
}

bool copy_tree(
    const std::filesystem::path& from,
    const std::filesystem::path& to) {
    std::error_code error;
    std::filesystem::copy(
        from,
        to,
        std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::overwrite_existing,
        error);
    return !error;
}

bool query_record_zero_fails_with(
    const std::filesystem::path& root,
    std::string_view expected,
    std::string* error) {
    LogicalNodeRecordIndexReader reader(root);
    LogicalNodeRecordIndexWindow result;
    if (!reader.open(error)) {
        return false;
    }
    if (reader.read_record(
            0U,
            kNoLogicalNodeRecordPosting,
            8U,
            &result,
            error)) {
        return false;
    }
    return error->find(expected) != std::string::npos;
}

bool test_store_binding_transplant_rejected() {
    const std::filesystem::path root = unique_root("record-index-store-binding");
    RootCleanup cleanup(root);
    const std::filesystem::path store_a = root / "a";
    const std::filesystem::path store_b = root / "b";
    std::string error;

    if (!require(build_store(store_a, false, &error), error) ||
        !require(build_arena(store_a, &error), error) ||
        !require(build_logical_node_record_index(store_a, &error), error) ||
        !require(build_store(store_b, true, &error), error) ||
        !require(copy_tree(
            store_a / "node-record-index-v1",
            store_b / "node-record-index-v1"),
            "copy index into same-payload/different-partition store")) {
        return false;
    }

    LogicalNodeRecordIndexReader reader(store_b);
    return require(!reader.open(&error), "transplanted index rejected") &&
        require(error.find("native-store binding mismatch") != std::string::npos,
                "store binding mismatch is explicit");
}

bool test_entry_crc_corruption_rejected() {
    const std::filesystem::path root = unique_root("record-index-crc");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_store(root, false, &error), error) ||
        !require(build_arena(root, &error), error) ||
        !require(build_logical_node_record_index(root, &error), error)) {
        return false;
    }

    const std::filesystem::path index = root / "node-record-index-v1";
    if (!require(flip_byte(index / "records.bin", 0U),
                 "tamper records entry")) {
        return false;
    }
    {
        std::string query_error;
        if (!require(query_record_zero_fails_with(
                root, "record entry CRC", &query_error),
                "record entry CRC corruption rejected")) {
            return false;
        }
    }

    if (!require(rebuild_index(root, &error), error) ||
        !require(flip_byte(index / "heads.bin", 0U),
                 "tamper head entry")) {
        return false;
    }
    {
        std::string query_error;
        if (!require(query_record_zero_fails_with(
                root, "head entry CRC", &query_error),
                "head entry CRC corruption rejected")) {
            return false;
        }
    }

    if (!require(rebuild_index(root, &error), error) ||
        !require(flip_byte(index / "postings.bin", 0U),
                 "tamper posting entry")) {
        return false;
    }
    std::string query_error;
    return require(query_record_zero_fails_with(
               root, "posting entry CRC", &query_error),
            "posting entry CRC corruption rejected");
}

bool expect_truncated_table_rejected(
    const std::filesystem::path& root,
    std::string_view filename,
    std::string* error) {
    const std::filesystem::path path =
        root / "node-record-index-v1" / std::string(filename);
    const std::uintmax_t size = std::filesystem::file_size(path);
    if (size == 0U) {
        *error = "test table unexpectedly empty";
        return false;
    }
    std::filesystem::resize_file(path, size - 1U);
    LogicalNodeRecordIndexReader reader(root);
    if (reader.open(error)) {
        *error = "truncated table unexpectedly opened";
        return false;
    }
    return error->find("file size does not match manifest") != std::string::npos;
}

bool test_all_fixed_tables_reject_truncation() {
    const std::filesystem::path root = unique_root("record-index-truncate-all");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_store(root, false, &error), error) ||
        !require(build_arena(root, &error), error) ||
        !require(build_logical_node_record_index(root, &error), error) ||
        !require(expect_truncated_table_rejected(root, "records.bin", &error),
                 "truncated record table rejected") ||
        !require(rebuild_index(root, &error), error) ||
        !require(expect_truncated_table_rejected(root, "heads.bin", &error),
                 "truncated head table rejected") ||
        !require(rebuild_index(root, &error), error) ||
        !require(expect_truncated_table_rejected(root, "postings.bin", &error),
                 "truncated posting table rejected")) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!test_store_binding_transplant_rejected() ||
        !test_entry_crc_corruption_rejected() ||
        !test_all_fixed_tables_reject_truncation()) {
        return 1;
    }
    return 0;
}
