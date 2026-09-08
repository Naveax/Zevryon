#pragma once

#include "frame_budget_scheduler.hpp"
#include "zenith_semantic_node_window.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace zevryon::massivedoc {

struct ZenithSemanticRuntimeStats {
    std::uint64_t requests{0U};
    std::uint64_t successful_windows{0U};
    std::uint64_t failed_windows{0U};
    std::uint64_t ui_lane_rejections{0U};
    std::uint64_t truncated_windows{0U};
    std::uint64_t materialized_nodes{0U};
    std::uint64_t materialized_attributes{0U};
    std::uint64_t semantic_bytes{0U};
};

// Browser-runtime authority for bounded semantic-node materialization. Reads
// may touch disk, so UI-lane calls fail before opening the arena. Worker-lane
// calls lazily open one disk-backed reader and serialize access to it.
class ZenithSemanticRuntimeConsumer final {
public:
    explicit ZenithSemanticRuntimeConsumer(
        std::filesystem::path store_root,
        ZenithSemanticNodeWindowConfig config = {});
    ~ZenithSemanticRuntimeConsumer();

    ZenithSemanticRuntimeConsumer(const ZenithSemanticRuntimeConsumer&) = delete;
    ZenithSemanticRuntimeConsumer& operator=(const ZenithSemanticRuntimeConsumer&) = delete;
    ZenithSemanticRuntimeConsumer(ZenithSemanticRuntimeConsumer&&) = delete;
    ZenithSemanticRuntimeConsumer& operator=(ZenithSemanticRuntimeConsumer&&) = delete;

    bool read(
        std::uint64_t start_ordinal,
        ZenithSemanticNodeWindowResult* result,
        std::string* error);

    bool read_on_lane(
        FrameExecutionLane lane,
        std::uint64_t start_ordinal,
        ZenithSemanticNodeWindowResult* result,
        std::string* error);

    ZenithSemanticRuntimeStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zevryon::massivedoc
