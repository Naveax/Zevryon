#include "zenith_semantic_node_window.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace zevryon::massivedoc {
namespace {

bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool add_size(std::size_t* total, std::size_t amount) noexcept {
    if (total == nullptr || *total > std::numeric_limits<std::size_t>::max() - amount) {
        return false;
    }
    *total += amount;
    return true;
}

bool resolve_node_semantics(
    const LogicalNodeArenaReader& reader,
    const LogicalNodeRecord& record,
    ZenithSemanticNode* node,
    std::size_t* semantic_bytes,
    std::string* error) {
    if (node == nullptr || semantic_bytes == nullptr || error == nullptr) {
        return false;
    }
    node->record = record;
    if (!reader.resolve_semantic(LogicalSemanticKind::tag, record.tag_id, &node->tag, error) ||
        !reader.resolve_semantic(LogicalSemanticKind::role, record.role_id, &node->role, error) ||
        !reader.resolve_semantic(LogicalSemanticKind::style, record.style_id, &node->style, error)) {
        return false;
    }
    return add_size(semantic_bytes, node->tag.size()) &&
        add_size(semantic_bytes, node->role.size()) &&
        add_size(semantic_bytes, node->style.size());
}

} // namespace

struct ZenithSemanticNodeWindow::Impl {
    Impl(std::filesystem::path root, ZenithSemanticNodeWindowConfig window_config)
        : reader(std::move(root)), config(window_config) {}

    LogicalNodeArenaReader reader;
    ZenithSemanticNodeWindowConfig config;
    bool opened{false};
};

ZenithSemanticNodeWindow::ZenithSemanticNodeWindow(
    std::filesystem::path store_root,
    ZenithSemanticNodeWindowConfig config)
    : impl_(std::make_unique<Impl>(std::move(store_root), config)) {}

ZenithSemanticNodeWindow::~ZenithSemanticNodeWindow() = default;
ZenithSemanticNodeWindow::ZenithSemanticNodeWindow(ZenithSemanticNodeWindow&&) noexcept = default;
ZenithSemanticNodeWindow& ZenithSemanticNodeWindow::operator=(
    ZenithSemanticNodeWindow&&) noexcept = default;

bool ZenithSemanticNodeWindow::open(std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (impl_->opened) {
        return fail(error, "zenith semantic node window is already open");
    }
    if (!impl_->config.valid()) {
        return fail(error, "zenith semantic node window configuration is invalid");
    }
    if (!impl_->reader.open(error)) {
        return false;
    }
    impl_->opened = true;
    return true;
}

const LogicalNodeArenaManifest& ZenithSemanticNodeWindow::manifest() const noexcept {
    return impl_->reader.manifest();
}

bool ZenithSemanticNodeWindow::read(
    std::uint64_t start_ordinal,
    ZenithSemanticNodeWindowResult* result,
    std::string* error) const {
    if (result == nullptr || error == nullptr) {
        return false;
    }
    *result = ZenithSemanticNodeWindowResult{};
    if (!impl_->opened) {
        return fail(error, "zenith semantic node window is not open");
    }
    error->clear();

    const std::uint64_t node_count = impl_->reader.manifest().node_count;
    if (start_ordinal > node_count) {
        return fail(error, "zenith semantic node window start ordinal is out of range");
    }

    result->start_ordinal = start_ordinal;
    result->next_ordinal = start_ordinal;
    result->arena_node_count = node_count;
    if (start_ordinal == node_count) {
        return true;
    }

    const std::uint64_t remaining_nodes = node_count - start_ordinal;
    const std::size_t reserve_nodes = static_cast<std::size_t>(
        std::min<std::uint64_t>(
            remaining_nodes,
            static_cast<std::uint64_t>(impl_->config.maximum_nodes)));
    result->nodes.reserve(reserve_nodes);

    std::uint64_t ordinal = start_ordinal;
    while (ordinal < node_count && result->nodes.size() < impl_->config.maximum_nodes) {
        LogicalNodeRecord record;
        if (!impl_->reader.node_by_ordinal(ordinal, &record, error)) {
            return false;
        }
        if (record.attribute_count > impl_->config.maximum_attributes_per_node) {
            return fail(error, "zenith semantic node exceeds per-node attribute budget");
        }
        const std::size_t node_attribute_count = static_cast<std::size_t>(record.attribute_count);
        if (node_attribute_count > impl_->config.maximum_total_attributes) {
            if (!result->nodes.empty()) {
                result->truncated = true;
                result->next_ordinal = ordinal;
                return true;
            }
            return fail(error, "zenith semantic node exceeds total attribute budget by itself");
        }
        if (result->attribute_count >
            impl_->config.maximum_total_attributes - node_attribute_count) {
            result->truncated = true;
            result->next_ordinal = ordinal;
            return true;
        }

        ZenithSemanticNode candidate;
        candidate.attributes.reserve(node_attribute_count);
        std::size_t candidate_semantic_bytes = 0U;
        if (!resolve_node_semantics(
                impl_->reader,
                record,
                &candidate,
                &candidate_semantic_bytes,
                error)) {
            if (error->empty()) {
                *error = "zenith semantic node semantic-byte accounting overflow";
            }
            return false;
        }

        for (std::uint32_t relative = 0U; relative < record.attribute_count; ++relative) {
            if (record.attribute_offset >
                std::numeric_limits<std::uint64_t>::max() - relative) {
                return fail(error, "zenith semantic node attribute ordinal overflows");
            }
            LogicalNodeAttributeRecord stored_attribute;
            if (!impl_->reader.attribute_by_ordinal(
                    record.attribute_offset + relative,
                    &stored_attribute,
                    error)) {
                return false;
            }
            ZenithSemanticAttribute attribute;
            attribute.flags = stored_attribute.flags;
            if (!impl_->reader.resolve_semantic(
                    LogicalSemanticKind::attribute_name,
                    stored_attribute.name_id,
                    &attribute.name,
                    error) ||
                !impl_->reader.resolve_semantic(
                    LogicalSemanticKind::attribute_value,
                    stored_attribute.value_id,
                    &attribute.value,
                    error)) {
                return false;
            }
            if (!add_size(&candidate_semantic_bytes, attribute.name.size()) ||
                !add_size(&candidate_semantic_bytes, attribute.value.size())) {
                return fail(error, "zenith semantic node semantic-byte accounting overflows");
            }
            candidate.attributes.push_back(std::move(attribute));
        }

        if (candidate_semantic_bytes > impl_->config.maximum_semantic_bytes) {
            if (!result->nodes.empty()) {
                result->truncated = true;
                result->next_ordinal = ordinal;
                return true;
            }
            return fail(error, "zenith semantic node exceeds semantic-byte budget by itself");
        }
        if (result->semantic_bytes >
            impl_->config.maximum_semantic_bytes - candidate_semantic_bytes) {
            result->truncated = true;
            result->next_ordinal = ordinal;
            return true;
        }

        result->semantic_bytes += candidate_semantic_bytes;
        result->attribute_count += node_attribute_count;
        result->nodes.push_back(std::move(candidate));
        ++ordinal;
        result->next_ordinal = ordinal;
    }

    result->truncated = ordinal < node_count;
    result->next_ordinal = ordinal;
    return true;
}

} // namespace zevryon::massivedoc
