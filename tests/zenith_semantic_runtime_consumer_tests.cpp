#include "logical_node_arena.hpp"
#include "zenith_semantic_runtime_consumer.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using zevryon::massivedoc::FrameExecutionLane;
using zevryon::massivedoc::LogicalNodeArenaBuildConfig;
using zevryon::massivedoc::LogicalNodeArenaWriter;
using zevryon::massivedoc::LogicalNodeAttributeInput;
using zevryon::massivedoc::LogicalNodeInput;
using zevryon::massivedoc::ZenithSemanticNodeWindowConfig;
using zevryon::massivedoc::ZenithSemanticNodeWindowResult;
using zevryon::massivedoc::ZenithSemanticRuntimeConsumer;
using zevryon::massivedoc::kNoLogicalNodeOrdinal;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: zenith semantic runtime consumer: " << message << '\n';
        return false;
    }
    return true;
}

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

LogicalNodeArenaBuildConfig arena_config() {
    LogicalNodeArenaBuildConfig config;
    config.candidate_commit = "0123456789abcdef0123456789abcdef01234567";
    config.candidate_tree = "89abcdef0123456789abcdef0123456789abcdef";
    config.semantic_bucket_count = 64U;
    config.semantic_hash_bits = 64U;
    return config;
}

bool build_fixture(const std::filesystem::path& root, std::string* error) {
    LogicalNodeArenaWriter writer(root, arena_config());
    if (!writer.begin(error)) {
        return false;
    }
    const std::array<LogicalNodeAttributeInput, 1> attributes{{
        {"role-note", "root", 5U},
    }};
    return writer.append_node(
               LogicalNodeInput{
                   1U, 0U, 0U, 10U, kNoLogicalNodeOrdinal,
                   "main", "main", "display:block", 1U},
               attributes,
               error) &&
        writer.append_node(
            LogicalNodeInput{
                2U, 0U, 10U, 5U, 0U,
                "span", "text", "color:red", 2U},
            {},
            error) &&
        writer.finish(error);
}

bool test_ui_rejection_then_worker_lazy_open() {
    const std::filesystem::path root = unique_root("semantic-runtime-lanes");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_fixture(root, &error), error)) {
        return false;
    }

    ZenithSemanticNodeWindowConfig config;
    config.maximum_nodes = 1U;
    ZenithSemanticRuntimeConsumer consumer(root, config);
    ZenithSemanticNodeWindowResult result;
    if (!require(!consumer.read_on_lane(
            FrameExecutionLane::Ui, 0U, &result, &error),
            "UI lane rejected before semantic I/O") ||
        !require(consumer.read(0U, &result, &error), error) ||
        !require(result.nodes.size() == 1U, "worker materializes one bounded node") ||
        !require(result.nodes[0].tag == "main", "worker resolves interned tag") ||
        !require(result.nodes[0].role == "main", "worker resolves interned role") ||
        !require(result.nodes[0].style == "display:block", "worker resolves interned style") ||
        !require(result.nodes[0].attributes.size() == 1U, "worker resolves attribute slice") ||
        !require(result.truncated && result.next_ordinal == 1U,
                 "worker preserves bounded continuation")) {
        return false;
    }

    const auto stats = consumer.stats();
    return require(stats.requests == 2U, "request telemetry") &&
        require(stats.ui_lane_rejections == 1U, "UI rejection telemetry") &&
        require(stats.successful_windows == 1U, "success telemetry") &&
        require(stats.failed_windows == 0U, "no worker failure") &&
        require(stats.truncated_windows == 1U, "truncation telemetry") &&
        require(stats.materialized_nodes == 1U, "materialized node telemetry") &&
        require(stats.materialized_attributes == 1U, "attribute telemetry") &&
        require(stats.semantic_bytes > 0U, "semantic byte telemetry");
}

bool test_missing_arena_and_invalid_config_fail_closed() {
    const std::filesystem::path root = unique_root("semantic-runtime-missing");
    RootCleanup cleanup(root);
    std::string error;
    ZenithSemanticNodeWindowResult result;

    ZenithSemanticRuntimeConsumer missing(root);
    if (!require(!missing.read(0U, &result, &error), "missing arena fails closed") ||
        !require(missing.stats().failed_windows == 1U, "missing arena failure telemetry")) {
        return false;
    }

    ZenithSemanticNodeWindowConfig invalid;
    invalid.maximum_nodes = 0U;
    ZenithSemanticRuntimeConsumer invalid_consumer(root, invalid);
    return require(!invalid_consumer.read(0U, &result, &error), "invalid config fails closed") &&
        require(invalid_consumer.stats().failed_windows == 1U, "invalid config telemetry");
}

} // namespace

int main() {
    if (!test_ui_rejection_then_worker_lazy_open() ||
        !test_missing_arena_and_invalid_config_fail_closed()) {
        return 1;
    }
    return 0;
}
