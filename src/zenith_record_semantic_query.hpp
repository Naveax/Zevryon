#pragma once

#include "logical_node_record_index.hpp"
#include "zenith_semantic_node_window.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace zevryon::massivedoc {

struct ZenithRecordSemanticNode {
    LogicalNodeRecordPosting source_overlap{};
    ZenithSemanticNode semantic;
};

struct ZenithRecordSemanticWindowResult {
    std::uint64_t source_record_index{0U};
    std::uint64_t next_posting_ordinal{kNoLogicalNodeRecordPosting};
    std::size_t semantic_bytes{0U};
    std::size_t attribute_count{0U};
    bool truncated{false};
    std::vector<ZenithRecordSemanticNode> nodes;
};

} // namespace zevryon::massivedoc
