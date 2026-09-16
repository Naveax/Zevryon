#pragma once

#include "logical_node_arena_v2_store_bound.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>

namespace zevryon::massivedoc {

struct LogicalDomTraversalConfig {
    static constexpr std::uint64_t kMaximumHopLimit = 16U * 1024U * 1024U;
    std::uint64_t maximum_hops{1U * 1024U * 1024U};

    bool valid() const noexcept {
        return maximum_hops > 0U && maximum_hops <= kMaximumHopLimit;
    }
};

struct LogicalDomTraversalResult {
    bool found{false};
    std::uint64_t ordinal{0U};
    LogicalNodeRecord record{};
};

class LogicalDomTraversal final {
public:
    explicit LogicalDomTraversal(
        std::filesystem::path store_root,
        LogicalDomTraversalConfig config = {})
        : reader_(std::move(store_root)), config_(config) {}

    LogicalDomTraversal(const LogicalDomTraversal&) = delete;
    LogicalDomTraversal& operator=(const LogicalDomTraversal&) = delete;
    LogicalDomTraversal(LogicalDomTraversal&&) noexcept = default;
    LogicalDomTraversal& operator=(LogicalDomTraversal&&) noexcept = default;

    bool open(std::string* error) {
        if (opened_) {
            return fail(error, "logical DOM traversal is already open");
        }
        if (!config_.valid()) {
            return fail(error, "logical DOM traversal configuration is invalid");
        }
        if (!reader_.open(error)) {
            return false;
        }
        opened_ = true;
        return true;
    }

    std::uint64_t node_count() const noexcept {
        return opened_ ? reader_.manifest().storage_manifest.node_count : 0U;
    }

    bool node(
        std::uint64_t ordinal,
        LogicalDomTraversalResult* result,
        std::string* error) const {
        if (!prepare_result(result, error)) {
            return false;
        }
        return read_found(ordinal, result, error);
    }

    bool parent(
        std::uint64_t ordinal,
        LogicalDomTraversalResult* result,
        std::string* error) const {
        if (!prepare_result(result, error)) {
            return false;
        }
        LogicalNodeRecord current;
        if (!read_record(ordinal, &current, error)) {
            return false;
        }
        if (current.parent_ordinal == kNoLogicalNodeOrdinal) {
            return true;
        }
        return read_found(current.parent_ordinal, result, error);
    }

    bool first_child(
        std::uint64_t ordinal,
        LogicalDomTraversalResult* result,
        std::string* error) const {
        if (!prepare_result(result, error)) {
            return false;
        }
        LogicalNodeRecord current;
        if (!read_record(ordinal, &current, error)) {
            return false;
        }
        if (current.first_child_ordinal == kNoLogicalNodeOrdinal) {
            return true;
        }
        if (!read_found(current.first_child_ordinal, result, error)) {
            return false;
        }
        if (result->record.parent_ordinal != ordinal) {
            clear(result);
            return fail(error, "logical DOM first-child parent topology mismatch");
        }
        return true;
    }

    bool next_sibling(
        std::uint64_t ordinal,
        LogicalDomTraversalResult* result,
        std::string* error) const {
        if (!prepare_result(result, error)) {
            return false;
        }
        LogicalNodeRecord current;
        if (!read_record(ordinal, &current, error)) {
            return false;
        }
        if (current.next_sibling_ordinal == kNoLogicalNodeOrdinal) {
            return true;
        }
        if (!read_found(current.next_sibling_ordinal, result, error)) {
            return false;
        }
        if (result->record.parent_ordinal != current.parent_ordinal) {
            clear(result);
            return fail(error, "logical DOM sibling parent topology mismatch");
        }
        return true;
    }

    bool previous_sibling(
        std::uint64_t ordinal,
        LogicalDomTraversalResult* result,
        std::string* error) const {
        if (!prepare_result(result, error)) {
            return false;
        }
        LogicalNodeRecord current;
        if (!read_record(ordinal, &current, error)) {
            return false;
        }
        if (current.parent_ordinal == kNoLogicalNodeOrdinal) {
            return true;
        }

        LogicalNodeRecord parent_record;
        if (!read_record(current.parent_ordinal, &parent_record, error)) {
            return false;
        }
        std::uint64_t cursor = parent_record.first_child_ordinal;
        if (cursor == kNoLogicalNodeOrdinal) {
            return fail(error, "logical DOM parent has no first child for child node");
        }
        if (cursor == ordinal) {
            return true;
        }

        for (std::uint64_t hops = 0U; hops < config_.maximum_hops; ++hops) {
            LogicalNodeRecord sibling;
            if (!read_record(cursor, &sibling, error)) {
                return false;
            }
            if (sibling.parent_ordinal != current.parent_ordinal) {
                return fail(error, "logical DOM sibling chain escapes parent");
            }
            if (sibling.next_sibling_ordinal == ordinal) {
                return read_found(cursor, result, error);
            }
            if (sibling.next_sibling_ordinal == kNoLogicalNodeOrdinal) {
                return fail(error, "logical DOM sibling chain does not reach requested node");
            }
            cursor = sibling.next_sibling_ordinal;
        }
        return fail(error, "logical DOM previous-sibling traversal exceeded hop budget");
    }

    bool next_preorder(
        std::uint64_t ordinal,
        LogicalDomTraversalResult* result,
        std::string* error) const {
        if (!prepare_result(result, error)) {
            return false;
        }
        LogicalNodeRecord current;
        if (!read_record(ordinal, &current, error)) {
            return false;
        }
        if (current.first_child_ordinal != kNoLogicalNodeOrdinal) {
            if (!read_found(current.first_child_ordinal, result, error)) {
                return false;
            }
            if (result->record.parent_ordinal != ordinal) {
                clear(result);
                return fail(error, "logical DOM preorder child topology mismatch");
            }
            return true;
        }

        std::uint64_t current_ordinal = ordinal;
        for (std::uint64_t hops = 0U; hops < config_.maximum_hops; ++hops) {
            if (current.next_sibling_ordinal != kNoLogicalNodeOrdinal) {
                if (!read_found(current.next_sibling_ordinal, result, error)) {
                    return false;
                }
                if (result->record.parent_ordinal != current.parent_ordinal) {
                    clear(result);
                    return fail(error, "logical DOM preorder sibling topology mismatch");
                }
                return true;
            }
            if (current.parent_ordinal == kNoLogicalNodeOrdinal) {
                return true;
            }
            current_ordinal = current.parent_ordinal;
            if (!read_record(current_ordinal, &current, error)) {
                return false;
            }
        }
        return fail(error, "logical DOM preorder traversal exceeded hop budget");
    }

    bool previous_preorder(
        std::uint64_t ordinal,
        LogicalDomTraversalResult* result,
        std::string* error) const {
        if (!prepare_result(result, error)) {
            return false;
        }
        LogicalNodeRecord current;
        if (!read_record(ordinal, &current, error)) {
            return false;
        }
        if (current.parent_ordinal == kNoLogicalNodeOrdinal) {
            return true;
        }

        LogicalDomTraversalResult previous;
        if (!previous_sibling(ordinal, &previous, error)) {
            return false;
        }
        if (!previous.found) {
            return read_found(current.parent_ordinal, result, error);
        }

        std::uint64_t cursor_ordinal = previous.ordinal;
        LogicalNodeRecord cursor = previous.record;
        std::uint64_t hops = 0U;
        while (cursor.first_child_ordinal != kNoLogicalNodeOrdinal) {
            if (++hops > config_.maximum_hops) {
                return fail(error, "logical DOM reverse preorder traversal exceeded hop budget");
            }
            std::uint64_t child_ordinal = cursor.first_child_ordinal;
            LogicalNodeRecord child;
            if (!read_record(child_ordinal, &child, error)) {
                return false;
            }
            if (child.parent_ordinal != cursor_ordinal) {
                return fail(error, "logical DOM reverse preorder child topology mismatch");
            }
            while (child.next_sibling_ordinal != kNoLogicalNodeOrdinal) {
                if (++hops > config_.maximum_hops) {
                    return fail(error, "logical DOM reverse preorder sibling scan exceeded hop budget");
                }
                const std::uint64_t sibling_ordinal = child.next_sibling_ordinal;
                LogicalNodeRecord sibling;
                if (!read_record(sibling_ordinal, &sibling, error)) {
                    return false;
                }
                if (sibling.parent_ordinal != cursor_ordinal) {
                    return fail(error, "logical DOM reverse preorder sibling topology mismatch");
                }
                child_ordinal = sibling_ordinal;
                child = sibling;
            }
            cursor_ordinal = child_ordinal;
            cursor = child;
        }
        return read_found(cursor_ordinal, result, error);
    }

    bool is_strict_descendant(
        std::uint64_t ordinal,
        std::uint64_t possible_ancestor,
        bool* result,
        std::string* error) const {
        if (result == nullptr) {
            return fail(error, "logical DOM descendant output is null");
        }
        *result = false;
        LogicalNodeRecord current;
        if (!read_record(ordinal, &current, error)) {
            return false;
        }
        LogicalNodeRecord ancestor_record;
        if (!read_record(possible_ancestor, &ancestor_record, error)) {
            return false;
        }
        (void)ancestor_record;
        if (ordinal == possible_ancestor) {
            return true;
        }
        for (std::uint64_t hops = 0U; hops < config_.maximum_hops; ++hops) {
            if (current.parent_ordinal == kNoLogicalNodeOrdinal) {
                return true;
            }
            if (current.parent_ordinal == possible_ancestor) {
                *result = true;
                return true;
            }
            if (!read_record(current.parent_ordinal, &current, error)) {
                return false;
            }
        }
        return fail(error, "logical DOM descendant traversal exceeded hop budget");
    }

    bool compare_document_order(
        std::uint64_t left,
        std::uint64_t right,
        int* result,
        std::string* error) const {
        if (result == nullptr) {
            return fail(error, "logical DOM document-order output is null");
        }
        LogicalNodeRecord left_record;
        LogicalNodeRecord right_record;
        if (!read_record(left, &left_record, error) ||
            !read_record(right, &right_record, error)) {
            return false;
        }
        (void)left_record;
        (void)right_record;
        *result = left < right ? -1 : (left > right ? 1 : 0);
        return true;
    }

private:
    static bool fail(std::string* error, std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    }

    static void clear(LogicalDomTraversalResult* result) noexcept {
        if (result != nullptr) {
            *result = LogicalDomTraversalResult{};
        }
    }

    bool prepare_result(
        LogicalDomTraversalResult* result,
        std::string* error) const {
        if (result == nullptr) {
            return fail(error, "logical DOM traversal output is null");
        }
        clear(result);
        if (!opened_) {
            return fail(error, "logical DOM traversal is not open");
        }
        return true;
    }

    bool read_record(
        std::uint64_t ordinal,
        LogicalNodeRecord* record,
        std::string* error) const {
        if (!opened_) {
            return fail(error, "logical DOM traversal is not open");
        }
        if (record == nullptr) {
            return fail(error, "logical DOM record output is null");
        }
        return reader_.node_by_ordinal(ordinal, record, error);
    }

    bool read_found(
        std::uint64_t ordinal,
        LogicalDomTraversalResult* result,
        std::string* error) const {
        LogicalNodeRecord record;
        if (!read_record(ordinal, &record, error)) {
            return false;
        }
        result->found = true;
        result->ordinal = ordinal;
        result->record = record;
        return true;
    }

    LogicalNodeArenaV2StoreBoundReader reader_;
    LogicalDomTraversalConfig config_;
    bool opened_{false};
};

} // namespace zevryon::massivedoc
