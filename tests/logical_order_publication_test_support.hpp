#pragma once

#include "logical_order_persistence.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace zevryon_test {

inline bool run_logical_order_publication_tests() {
    using zevryon::massivedoc::LogicalOrderSnapshot;
    using zevryon::massivedoc::load_logical_order_snapshot;
    using zevryon::massivedoc::logical_order_generation_path;
    using zevryon::massivedoc::logical_order_generation_temp_path;
    using zevryon::massivedoc::publish_logical_order_snapshot;

    const auto require = [](bool condition, const std::string& message) {
        if (!condition) {
            std::cerr << "FAILED: logical order publication: " << message << '\n';
            return false;
        }
        return true;
    };

    const auto root = std::filesystem::temp_directory_path() /
                      "zevryon-logical-order-publication-tests";
    std::error_code fs_error;
    std::filesystem::remove_all(root, fs_error);
    fs_error.clear();
    std::filesystem::create_directories(root / "arena", fs_error);
    if (!require(!fs_error, "cannot create publication fixture")) {
        return false;
    }

    std::string error;
    const std::vector<std::uint64_t> generation1{2U, 0U, 1U};
    if (!require(
            publish_logical_order_snapshot(root, 1U, generation1, &error), error) ||
        !require(
            std::filesystem::exists(logical_order_generation_path(root, 1U)),
            "committed generation file exists") ||
        !require(
            !std::filesystem::exists(logical_order_generation_temp_path(root, 1U)),
            "committed generation leaves no temporary file")) {
        return false;
    }

    LogicalOrderSnapshot loaded;
    if (!require(load_logical_order_snapshot(root, 3U, &loaded, &error), error) ||
        !require(loaded.persisted, "published generation is authoritative") ||
        !require(loaded.generation == 1U, "published generation selected") ||
        !require(loaded.source_record_indices == generation1, "published order preserved")) {
        return false;
    }

    if (!require(
            !publish_logical_order_snapshot(root, 1U, generation1, &error),
            "same generation cannot be replaced") ||
        !require(
            !publish_logical_order_snapshot(root, 0U, generation1, &error),
            "generation zero rejected")) {
        return false;
    }

    const auto stale_temp = logical_order_generation_temp_path(root, 2U);
    {
        std::ofstream stream(stale_temp, std::ios::binary | std::ios::trunc);
        stream << "torn";
    }
    const std::vector<std::uint64_t> generation2{1U, 2U, 0U};
    if (!require(
            publish_logical_order_snapshot(root, 2U, generation2, &error), error) ||
        !require(
            !std::filesystem::exists(stale_temp),
            "retry replaces stale temporary candidate") ||
        !require(load_logical_order_snapshot(root, 3U, &loaded, &error), error) ||
        !require(loaded.generation == 2U, "newer generation selected") ||
        !require(loaded.source_record_indices == generation2, "newer order preserved")) {
        return false;
    }

    const std::vector<std::uint64_t> invalid_order{0U, 0U, 2U};
    if (!require(
            !publish_logical_order_snapshot(root, 3U, invalid_order, &error),
            "invalid permutation rejected before commit") ||
        !require(
            !std::filesystem::exists(logical_order_generation_path(root, 3U)),
            "invalid permutation creates no committed generation")) {
        return false;
    }

    const std::vector<std::uint64_t> generation3{0U, 2U, 1U};
    if (!require(
            publish_logical_order_snapshot(root, 3U, generation3, &error), error) ||
        !require(load_logical_order_snapshot(root, 3U, &loaded, &error), error) ||
        !require(loaded.generation == 3U, "third generation selected") ||
        !require(loaded.source_record_indices == generation3, "third order preserved")) {
        return false;
    }

    std::filesystem::remove_all(root, fs_error);
    return require(!fs_error, "publication fixture cleanup");
}

} // namespace zevryon_test
