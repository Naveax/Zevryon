// Extend the admitted record-index translation unit without reopening the
// store-bound logical-node arena. The included implementation owns the exact
// arena instance and table handles used to validate posting overlap; the
// methods below expose bounded authority primitives from those same instances.
#include "logical_node_record_index_admitted.cpp"

namespace zevryon::massivedoc {
namespace {

bool fail_same_arena(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

} // namespace

bool LogicalNodeRecordIndexReader::read_head_snapshot(
    std::uint64_t source_record_index,
    LogicalNodeRecordIndexHeadSnapshot* head,
    std::string* error) const {
    if (head == nullptr || error == nullptr) {
        return false;
    }
    *head = LogicalNodeRecordIndexHeadSnapshot{};
    error->clear();
    if (!impl_->opened || impl_->heads == nullptr) {
        return fail_same_arena(
            error,
            "logical-node record-index same-handle head reader is not open");
    }
    if (source_record_index >= impl_->manifest.source_record_count) {
        return fail_same_arena(
            error,
            "logical-node record-index head snapshot source record is out of range");
    }

    HeadEntry stored;
    if (!read_head_entry_at(
            *impl_->heads,
            source_record_index,
            &stored,
            error)) {
        return false;
    }
    head->first_posting = stored.first_posting;
    head->last_posting = stored.last_posting;
    head->posting_count = stored.posting_count;
    return true;
}

bool LogicalNodeRecordIndexReader::node_by_ordinal(
    std::uint64_t ordinal,
    LogicalNodeRecord* node,
    std::string* error) const {
    if (node == nullptr || error == nullptr) {
        return false;
    }
    error->clear();
    if (!impl_->opened || impl_->arena == nullptr) {
        return fail_same_arena(
            error,
            "logical-node record-index same-arena reader is not open");
    }
    return impl_->arena->node_by_ordinal(ordinal, node, error);
}

bool LogicalNodeRecordIndexReader::attribute_by_ordinal(
    std::uint64_t ordinal,
    LogicalNodeAttributeRecord* attribute,
    std::string* error) const {
    if (attribute == nullptr || error == nullptr) {
        return false;
    }
    error->clear();
    if (!impl_->opened || impl_->arena == nullptr) {
        return fail_same_arena(
            error,
            "logical-node record-index same-arena reader is not open");
    }
    return impl_->arena->attribute_by_ordinal(ordinal, attribute, error);
}

bool LogicalNodeRecordIndexReader::resolve_semantic(
    LogicalSemanticKind kind,
    std::uint32_t id,
    std::string* value,
    std::string* error) const {
    if (value == nullptr || error == nullptr) {
        return false;
    }
    value->clear();
    error->clear();
    if (!impl_->opened || impl_->arena == nullptr) {
        return fail_same_arena(
            error,
            "logical-node record-index same-arena reader is not open");
    }
    return impl_->arena->resolve_semantic(kind, id, value, error);
}

} // namespace zevryon::massivedoc
