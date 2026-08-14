#pragma once

#include "logical_order_persistence.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace zevryon_test {
namespace {

bool logical_order_require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: logical order persistence: " << message << '\n';
        return false;
    }
    return true;
}

template <typename T>
void logical_order_append_le(std::vector<std::byte>* bytes, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t shift = 0U; shift < sizeof(T); ++shift) {
        bytes->push_back(static_cast<std::byte>(
            (value >> (shift * 8U)) & static_cast<T>(0xffU)));
    }
}

std::uint32_t logical_order_test_crc32(
    std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = ~std::uint32_t{0U};
    for (const std::byte raw : bytes) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(raw));
        for (std::uint32_t bit = 0U; bit < 8U; ++bit) {
            const std::uint32_t mask = static_cast<std::uint32_t>(
                -static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

std::vector<std::byte> make_logical_order_snapshot_bytes(
    std::uint64_t generation,
    const std::vector<std::uint64_t>& order) {
    std::vector<std::byte> bytes;
    bytes.reserve(44U + order.size() * sizeof(std::uint64_t));
    for (const char value : std::string{"ZVORD001"}) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    logical_order_append_le(&bytes, std::uint32_t{1U});
    logical_order_append_le(&bytes, std::uint32_t{40U});
    logical_order_append_le(&bytes, static_cast<std::uint64_t>(order.size()));
    logical_order_append_le(&bytes, generation);
    logical_order_append_le(
        &bytes,
        static_cast<std::uint64_t>(order.size()) * sizeof(std::uint64_t));
    for (const std::uint64_t source_record_index : order) {
        logical_order_append_le(&bytes, source_record_index);
    }
    logical_order_append_le(
        &bytes,
        logical_order_test_crc32(std::span<const std::byte>(bytes)));
    return bytes;
}

bool write_logical_order_test_file(
    const std::filesystem::path& path,
    std::span<const std::byte> bytes,
    std::string* error) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        *error = "cannot create logical order test file";
        return false;
    }
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream) {
        *error = "cannot write logical order test file";
        return false;
    }
    return true;
}

} // namespace

inline bool run_logical_order_persistence_tests() {
    using zevryon::massivedoc::LogicalOrderSnapshot;
    using zevryon::massivedoc::load_logical_order_snapshot;
    using zevryon::massivedoc::logical_order_generation_path;
    using zevryon::massivedoc::logical_order_generation_temp_path;
    using zevryon::massivedoc::parse_logical_order_snapshot;

    const auto root = std::filesystem::temp_directory_path() /
                      "zevryon-logical-order-persistence-tests";
    std::error_code fs_error;
    std::filesystem::remove_all(root, fs_error);
    fs_error.clear();
    std::filesystem::create_directories(root / "arena", fs_error);
    if (!logical_order_require(!fs_error, "cannot create fixture directory")) {
        return false;
    }

    std::string error;
    LogicalOrderSnapshot identity;
    if (!logical_order_require(
            load_logical_order_snapshot(root, 3U, &identity, &error), error) ||
        !logical_order_require(!identity.persisted, "no committed generation uses identity order") ||
        !logical_order_require(identity.generation == 0U, "identity order has generation zero") ||
        !logical_order_require(
            identity.source_record_indices == std::vector<std::uint64_t>({0U, 1U, 2U}),
            "identity fallback preserves physical order")) {
        return false;
    }

    const auto generation7 = make_logical_order_snapshot_bytes(7U, {2U, 0U, 1U});
    LogicalOrderSnapshot parsed;
    if (!logical_order_require(
            parse_logical_order_snapshot(generation7, 3U, &parsed, &error), error) ||
        !logical_order_require(parsed.persisted, "valid snapshot is authoritative") ||
        !logical_order_require(parsed.generation == 7U, "generation parsed") ||
        !logical_order_require(
            parsed.source_record_indices == std::vector<std::uint64_t>({2U, 0U, 1U}),
            "permutation parsed exactly") ||
        !logical_order_require(
            write_logical_order_test_file(
                logical_order_generation_path(root, 7U), generation7, &error),
            error)) {
        return false;
    }

    LogicalOrderSnapshot loaded;
    if (!logical_order_require(
            load_logical_order_snapshot(root, 3U, &loaded, &error), error) ||
        !logical_order_require(loaded.generation == 7U, "single committed generation selected")) {
        return false;
    }

    auto torn9 = make_logical_order_snapshot_bytes(9U, {1U, 2U, 0U});
    torn9.resize(torn9.size() / 2U);
    if (!logical_order_require(
            write_logical_order_test_file(
                logical_order_generation_temp_path(root, 9U), torn9, &error),
            error) ||
        !logical_order_require(
            load_logical_order_snapshot(root, 3U, &loaded, &error), error) ||
        !logical_order_require(
            loaded.generation == 7U,
            "torn temporary generation is ignored")) {
        return false;
    }

    const auto generation8 = make_logical_order_snapshot_bytes(8U, {1U, 2U, 0U});
    if (!logical_order_require(
            write_logical_order_test_file(
                logical_order_generation_path(root, 8U), generation8, &error),
            error) ||
        !logical_order_require(
            load_logical_order_snapshot(root, 3U, &loaded, &error), error) ||
        !logical_order_require(loaded.generation == 8U, "highest committed generation selected") ||
        !logical_order_require(
            loaded.source_record_indices == std::vector<std::uint64_t>({1U, 2U, 0U}),
            "highest generation order selected")) {
        return false;
    }

    auto corrupt10 = make_logical_order_snapshot_bytes(10U, {0U, 2U, 1U});
    corrupt10.back() ^= std::byte{0x01U};
    const auto corrupt10_path = logical_order_generation_path(root, 10U);
    if (!logical_order_require(
            write_logical_order_test_file(corrupt10_path, corrupt10, &error), error) ||
        !logical_order_require(
            !load_logical_order_snapshot(root, 3U, &loaded, &error),
            "corrupt committed generation fails closed")) {
        return false;
    }
    std::filesystem::remove(corrupt10_path, fs_error);
    if (!logical_order_require(!fs_error, "remove corrupt committed generation")) {
        return false;
    }

    const auto mismatched11 = make_logical_order_snapshot_bytes(12U, {0U, 1U, 2U});
    const auto mismatched11_path = logical_order_generation_path(root, 11U);
    if (!logical_order_require(
            write_logical_order_test_file(mismatched11_path, mismatched11, &error), error) ||
        !logical_order_require(
            !load_logical_order_snapshot(root, 3U, &loaded, &error),
            "filename/header generation mismatch rejected")) {
        return false;
    }
    std::filesystem::remove(mismatched11_path, fs_error);
    if (!logical_order_require(!fs_error, "remove mismatched generation")) {
        return false;
    }

    auto truncated = generation7;
    truncated.pop_back();
    if (!logical_order_require(
            !parse_logical_order_snapshot(truncated, 3U, &parsed, &error),
            "truncated snapshot rejected")) {
        return false;
    }

    auto bad_magic = generation7;
    bad_magic[0] = static_cast<std::byte>(static_cast<unsigned char>('X'));
    if (!logical_order_require(
            !parse_logical_order_snapshot(bad_magic, 3U, &parsed, &error),
            "bad magic rejected") ||
        !logical_order_require(
            !parse_logical_order_snapshot(generation7, 4U, &parsed, &error),
            "record-count mismatch rejected")) {
        return false;
    }

    const auto duplicate = make_logical_order_snapshot_bytes(13U, {2U, 0U, 0U});
    const auto out_of_range = make_logical_order_snapshot_bytes(14U, {2U, 0U, 3U});
    if (!logical_order_require(
            !parse_logical_order_snapshot(duplicate, 3U, &parsed, &error),
            "duplicate source rejected") ||
        !logical_order_require(
            !parse_logical_order_snapshot(out_of_range, 3U, &parsed, &error),
            "out-of-range source rejected")) {
        return false;
    }

    std::filesystem::remove_all(root, fs_error);
    return logical_order_require(!fs_error, "fixture cleanup");
}

} // namespace zevryon_test
