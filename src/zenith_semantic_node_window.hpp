#pragma once

#include "logical_node_arena.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace zevryon::massivedoc {

struct ZenithSemanticNodeWindowConfig {
    static constexpr std::size_t kMaximumNodesLimit = 65'536U;
    static constexpr std::uint32_t kMaximumAttributesPerNodeLimit = 65'536U;
    static constexpr std::size_t kMaximumTotalAttributesLimit = 1'048'576U;
    static constexpr std::size_t kMaximumSemanticBytesLimit = 256U * 1024U * 1024U;

    std::size_t maximum_nodes{256U};
    std::uint32_t maximum_attributes_per_node{64U};
    std::size_t maximum_total_attributes{4096U};
    std::size_t maximum_semantic_bytes{4U * 1024U * 1024U};

    bool valid() const noexcept {
        return maximum_nodes > 0U && maximum_nodes <= kMaximumNodesLimit &&
            maximum_attributes_per_node > 0U &&
            maximum_attributes_per_node <= kMaximumAttributesPerNodeLimit &&
            maximum_total_attributes >= maximum_attributes_per_node &&
            maximum_total_attributes <= kMaximumTotalAttributesLimit &&
            maximum_semantic_bytes > 0U &&
            maximum_semantic_bytes <= kMaximumSemanticBytesLimit;
    }
};

struct ZenithSemanticAttribute {
    std::string name;
    std::string value;
    std::uint32_t flags{0U};
};

struct ZenithSemanticNode {
    LogicalNodeRecord record{};
    std::string tag;
    std::string role;
    std::string style;
    std::vector<ZenithSemanticAttribute> attributes;
};

struct ZenithSemanticNodeWindowResult {
    std::uint64_t start_ordinal{0U};
    std::uint64_t next_ordinal{0U};
    std::uint64_t arena_node_count{0U};
    std::size_t semantic_bytes{0U};
    std::size_t attribute_count{0U};
    bool truncated{false};
    std::vector<ZenithSemanticNode> nodes;
};

// Production browser-runtime consumer of the disk-backed logical-node arena.
// It never materializes the complete node graph. Each call returns complete
// node units from one bounded ordinal window, including topology, source
// identity, tag/role/style and attributes.
class ZenithSemanticNodeWindow final {
public:
    explicit ZenithSemanticNodeWindow(
        std::filesystem::path store_root,
        ZenithSemanticNodeWindowConfig config = {});
    ~ZenithSemanticNodeWindow();

    ZenithSemanticNodeWindow(const ZenithSemanticNodeWindow&) = delete;
    ZenithSemanticNodeWindow& operator=(const ZenithSemanticNodeWindow&) = delete;
    ZenithSemanticNodeWindow(ZenithSemanticNodeWindow&&) noexcept;
    ZenithSemanticNodeWindow& operator=(ZenithSemanticNodeWindow&&) noexcept;

    bool open(std::string* error);
    const LogicalNodeArenaManifest& manifest() const noexcept;

    // start_ordinal == manifest().node_count is a valid empty end window.
    // A result is truncated only at a complete-node boundary. If one node by
    // itself exceeds a configured per-node or semantic budget the read fails
    // closed instead of returning incomplete browser semantics.
    bool read(
        std::uint64_t start_ordinal,
        ZenithSemanticNodeWindowResult* result,
        std::string* error) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zevryon::massivedoc
