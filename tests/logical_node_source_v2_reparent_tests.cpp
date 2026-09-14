#include "logical_node_source.hpp"
#include "logical_node_source_v2.hpp"
#include "logical_node_source_v2_reparent.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using zevryon::massivedoc::LogicalNodeInput;
using zevryon::massivedoc::LogicalNodeSourceNode;
using zevryon::massivedoc::LogicalNodeSourceStoreBinding;
using zevryon::massivedoc::LogicalNodeSourceV2Reader;
using zevryon::massivedoc::LogicalNodeSourceV2Writer;
using zevryon::massivedoc::kNoLogicalNodeOrdinal;
using zevryon::massivedoc::reparent_logical_node_source_v2_staging_file;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: logical node source v2 reparent: " << message << '\n';
        return false;
    }
    return true;
}

std::filesystem::path unique_path() {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (std::string("zevryon-v2-reparent-") + std::to_string(tick) + ".zvnsrc");
}

struct Cleanup {
    explicit Cleanup(std::filesystem::path value) : path(std::move(value)) {}
    ~Cleanup() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    std::filesystem::path path;
};

bool append(LogicalNodeSourceV2Writer* writer,
            std::uint64_t id,
            std::uint64_t parent,
            std::string_view tag,
            std::string* error) {
    return writer->append_node(
        LogicalNodeInput{id, 0U, 0U, 0U, parent, tag, "", "", 0U},
        {}, error);
}

} // namespace

int main() {
    const auto path = unique_path();
    Cleanup cleanup(path);
    std::string error;
    LogicalNodeSourceV2Writer writer(path);
    if (!require(writer.begin(&error), error) ||
        !require(append(&writer, 1U, kNoLogicalNodeOrdinal, "#document", &error), error) ||
        !require(append(&writer, 2U, 0U, "a", &error), error) ||
        !require(append(&writer, 3U, 1U, "b", &error), error) ||
        !require(append(&writer, 4U, 2U, "#text", &error), error)) {
        return 1;
    }

    LogicalNodeSourceStoreBinding binding;
    binding.source_record_count = 1U;
    if (!require(writer.finish(binding, &error), error)) {
        return 1;
    }

    if (!require(
            !reparent_logical_node_source_v2_staging_file(path, 0U, 0U, &error),
            "document reparent unexpectedly succeeded") ||
        !require(
            !reparent_logical_node_source_v2_staging_file(path, 2U, 2U, &error),
            "forward/self parent unexpectedly succeeded") ||
        !require(
            reparent_logical_node_source_v2_staging_file(path, 2U, 0U, &error),
            error) ||
        !require(
            reparent_logical_node_source_v2_staging_file(path, 3U, 1U, &error),
            error)) {
        return 1;
    }

    LogicalNodeSourceV2Reader reader(path);
    if (!require(reader.open(&error), error)) {
        return 1;
    }
    for (std::uint64_t ordinal = 0U; ordinal < 4U; ++ordinal) {
        LogicalNodeSourceNode node;
        bool has_node = false;
        if (!require(reader.next(&node, &has_node, &error), error) ||
            !require(has_node, "reader ended before all patched nodes")) {
            return 1;
        }
        if (ordinal == 2U &&
            !require(node.parent_ordinal == 0U, "first reparent was not persisted")) {
            return 1;
        }
        if (ordinal == 3U &&
            !require(node.parent_ordinal == 1U, "second reparent was not persisted")) {
            return 1;
        }
    }
    LogicalNodeSourceNode extra;
    bool has_extra = true;
    if (!require(reader.next(&extra, &has_extra, &error), error) ||
        !require(!has_extra, "reader found trailing node after patched stream")) {
        return 1;
    }

    std::cout << "Z7 staged node-source reparent PASS\n";
    return 0;
}
