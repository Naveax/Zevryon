// Compile the authoritative record-index implementation in this translation
// unit, then expose semantic primitives through its already-open low-level
// storage reader. This keeps posting authority and browser semantics bound to
// one validated arena instance.
#include "logical_node_record_index_authoritative.cpp"

namespace zevryon::massivedoc {
namespace {

bool fail_authoritative_semantic(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

} // namespace

bool LogicalNodeRecordIndexAuthoritativeReader::node_by_ordinal(
    std::uint64_t ordinal,
    LogicalNodeRecord* node,
    std::string* error) const {
    if (node == nullptr || error == nullptr) {
        return false;
    }
    error->clear();
    if (!impl_->opened || impl_->storage == nullptr) {
        return fail_authoritative_semantic(
            error,
            "logical-node authoritative same-arena reader is not open");
    }
    return impl_->storage->node_by_ordinal(ordinal, node, error);
}

bool LogicalNodeRecordIndexAuthoritativeReader::attribute_by_ordinal(
    std::uint64_t ordinal,
    LogicalNodeAttributeRecord* attribute,
    std::string* error) const {
    if (attribute == nullptr || error == nullptr) {
        return false;
    }
    error->clear();
    if (!impl_->opened || impl_->storage == nullptr) {
        return fail_authoritative_semantic(
            error,
            "logical-node authoritative same-arena reader is not open");
    }
    return impl_->storage->attribute_by_ordinal(ordinal, attribute, error);
}

bool LogicalNodeRecordIndexAuthoritativeReader::resolve_semantic(
    LogicalSemanticKind kind,
    std::uint32_t id,
    std::string* value,
    std::string* error) const {
    if (value == nullptr || error == nullptr) {
        return false;
    }
    value->clear();
    error->clear();
    if (!impl_->opened || impl_->storage == nullptr) {
        return fail_authoritative_semantic(
            error,
            "logical-node authoritative same-arena reader is not open");
    }
    return impl_->storage->resolve_semantic(kind, id, value, error);
}

} // namespace zevryon::massivedoc
