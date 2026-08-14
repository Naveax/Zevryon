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
    using zevryon::massivedoc::logical_order_snapshot_path;
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
        !logical_order_require(!identity.persisted, "absent sidecar uses identity order") ||
        !logical_order_require(identity.generation == 0U, "identity order has generation zero") ||
        !logical_order_require(
            identity.source_record_indices == std::vector<std::uint64_t>({0U, 1U, 2U}),
            "identity fallback preserves physical order")) {
        return false;
    }

    const auto valid = make_logical_order_snapshot_bytes(7U, {2U, 0U, 1U});
    LogicalOrderSnapshot parsed;
    if (!logical_order_require(
            parse_logical_order_snapshot(valid, 3U, &parsed, &error), error) ||
        !logical_order_require(parsed.persisted, "valid sidecar is authoritative") ||
        !logical_order_require(parsed.generation == 7U, "generation parsed") ||
        !logical_order_require(
            parsed.source_record_indices == std::vector<std::uint64_t>({2U, 0U, 1U}),
            "permutation parsed exactly")) {
        return false;
    }

    const auto path = logical_order_snapshot_path(root);
    if (!logical_order_require(
            write_logical_order_test_file(path, valid, &error), error)) {
        return false;
    }
    LogicalOrderSnapshot loaded;
    if (!logical_order_require(
            load_logical_order_snapshot(root, 3U, &loaded, &error), error) ||
        !logical_order_require(loaded.persisted, "present valid sidecar loads") ||
        !logical_order_require(loaded.generation == 7U, "file generation preserved") ||
        !logical_order_require(
            loaded.source_record_indices == parsed.source_record_indices,
            "file loader preserves parsed permutation")) {
        return false;
    }

    auto truncated = valid;
    truncated.pop_back();
    if (!logical_order_require(
            !parse_logical_order_snapshot(truncated, 3U, &parsed, &error),
            "truncated sidecar rejected")) {
        return false;
    }

    auto bad_magic = valid;
    bad_magic[0] = static_cast<std::byte>(static_cast<unsigned char>('X'));
    if (!logical_order_require(
            !parse_logical_order_snapshot(bad_magic, 3U, &parsed, &error),
            "bad magic rejected")) {
        return false;
    }

    if (!logical_order_require(
            !parse_logical_order_snapshot(valid, 4U, &parsed, &error),
            "record-count mismatch rejected")) {
        return false;
    }

    const auto duplicate = make_logical_order_snapshot_bytes(8U, {2U, 0U, 0U});
    if (!logical_order_require(
            !parse_logical_order_snapshot(duplicate, 3U, &parsed, &error),
            "duplicate source rejected")) {
        return false;
    }

    const auto out_of_range = make_logical_order_snapshot_bytes(9U, {2U, 0U, 3U});
    if (!logical_order_require(
            !parse_logical_order_snapshot(out_of_range, 3U, &parsed, &error),
            "out-of-range source rejected")) {
        return false;
    }

    auto bad_crc = valid;
    bad_crc.back() ^= std::byte{0x01U};
    if (!logical_order_require(
            !parse_logical_order_snapshot(bad_crc, 3U, &parsed, &error),
            "CRC mismatch rejected")) {
        return false;
    }

    auto extra = valid;
    extra.push_back(std::byte{0U});
    if (!logical_order_require(
            !parse_logical_order_snapshot(extra, 3U, &parsed, &error),
            "trailing bytes rejected")) {
        return false;
    }

    if (!logical_order_require(
            write_logical_order_test_file(path, bad_crc, &error), error) ||
        !logical_order_require(
            !load_logical_order_snapshot(root, 3U, &loaded, &error),
            "corrupt present sidecar fails closed")) {
        return false;
    }

    std::filesystem::remove_all(root, fs_error);
    return logical_order_require(!fs_error, "fixture cleanup");
}

} // namespace zevryon_test
