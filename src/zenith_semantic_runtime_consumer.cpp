#include "zenith_semantic_runtime_consumer.hpp"

#include <limits>
#include <mutex>
#include <utility>

namespace zevryon::massivedoc {
namespace {

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
    return left > std::numeric_limits<std::uint64_t>::max() - right
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}

} // namespace

struct ZenithSemanticRuntimeConsumer::Impl {
    Impl(std::filesystem::path root_value, ZenithSemanticNodeWindowConfig window_config)
        : root(std::move(root_value)), config(window_config) {}

    std::filesystem::path root;
    ZenithSemanticNodeWindowConfig config;
    std::unique_ptr<ZenithSemanticNodeWindow> window;
    mutable std::mutex mutex;
    ZenithSemanticRuntimeStats statistics;

    bool ensure_open(std::string* error) {
        if (window != nullptr) {
            return true;
        }
        auto candidate = std::make_unique<ZenithSemanticNodeWindow>(root, config);
        if (!candidate->open(error)) {
            return false;
        }
        window = std::move(candidate);
        return true;
    }
};

ZenithSemanticRuntimeConsumer::ZenithSemanticRuntimeConsumer(
    std::filesystem::path store_root,
    ZenithSemanticNodeWindowConfig config)
    : impl_(std::make_unique<Impl>(std::move(store_root), config)) {}

ZenithSemanticRuntimeConsumer::~ZenithSemanticRuntimeConsumer() = default;

bool ZenithSemanticRuntimeConsumer::read(
    std::uint64_t start_ordinal,
    ZenithSemanticNodeWindowResult* result,
    std::string* error) {
    return read_on_lane(
        FrameExecutionLane::Worker,
        start_ordinal,
        result,
        error);
}

bool ZenithSemanticRuntimeConsumer::read_on_lane(
    FrameExecutionLane lane,
    std::uint64_t start_ordinal,
    ZenithSemanticNodeWindowResult* result,
    std::string* error) {
    if (result == nullptr || error == nullptr ||
        (lane != FrameExecutionLane::Ui && lane != FrameExecutionLane::Worker)) {
        if (error != nullptr) {
            *error = "invalid zenith semantic runtime request";
        }
        return false;
    }
    *result = ZenithSemanticNodeWindowResult{};
    error->clear();

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->statistics.requests = saturating_add(impl_->statistics.requests, 1U);

    if (lane == FrameExecutionLane::Ui) {
        impl_->statistics.ui_lane_rejections =
            saturating_add(impl_->statistics.ui_lane_rejections, 1U);
        *error = "blocking semantic-node arena I/O is forbidden on the UI execution lane";
        return false;
    }

    if (!impl_->config.valid()) {
        impl_->statistics.failed_windows =
            saturating_add(impl_->statistics.failed_windows, 1U);
        *error = "invalid zenith semantic runtime window configuration";
        return false;
    }
    if (!impl_->ensure_open(error) ||
        !impl_->window->read(start_ordinal, result, error)) {
        impl_->statistics.failed_windows =
            saturating_add(impl_->statistics.failed_windows, 1U);
        return false;
    }

    impl_->statistics.successful_windows =
        saturating_add(impl_->statistics.successful_windows, 1U);
    if (result->truncated) {
        impl_->statistics.truncated_windows =
            saturating_add(impl_->statistics.truncated_windows, 1U);
    }
    impl_->statistics.materialized_nodes = saturating_add(
        impl_->statistics.materialized_nodes,
        static_cast<std::uint64_t>(result->nodes.size()));
    impl_->statistics.materialized_attributes = saturating_add(
        impl_->statistics.materialized_attributes,
        static_cast<std::uint64_t>(result->attribute_count));
    impl_->statistics.semantic_bytes = saturating_add(
        impl_->statistics.semantic_bytes,
        static_cast<std::uint64_t>(result->semantic_bytes));
    return true;
}

ZenithSemanticRuntimeStats ZenithSemanticRuntimeConsumer::stats() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->statistics;
    } catch (...) {
        return ZenithSemanticRuntimeStats{};
    }
}

} // namespace zevryon::massivedoc
