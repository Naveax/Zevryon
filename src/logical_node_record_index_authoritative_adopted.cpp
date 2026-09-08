#include "logical_node_record_index.hpp"

#include <memory>
#include <string>
#include <utility>

namespace zevryon::massivedoc {
namespace {

bool fail_authoritative_adopted(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool validate_authoritative_head(
    const LogicalNodeRecordIndexManifest& manifest,
    const LogicalNodeRecordIndexHeadSnapshot& head,
    std::string* error) {
    if (head.posting_count == 0U) {
        if (head.first_posting != kNoLogicalNodeRecordPosting ||
            head.last_posting != kNoLogicalNodeRecordPosting) {
            return fail_authoritative_adopted(
                error,
                "logical-node record-index authoritative empty head has postings");
        }
        return true;
    }

    if (head.first_posting == kNoLogicalNodeRecordPosting ||
        head.last_posting == kNoLogicalNodeRecordPosting) {
        return fail_authoritative_adopted(
            error,
            "logical-node record-index authoritative non-empty head misses first/last posting");
    }
    if (head.first_posting > head.last_posting) {
        return fail_authoritative_adopted(
            error,
            "logical-node record-index authoritative head first posting exceeds last posting");
    }
    if (head.last_posting >= manifest.posting_count) {
        return fail_authoritative_adopted(
            error,
            "logical-node record-index authoritative head last posting is out of range");
    }
    const std::uint64_t ordinal_span =
        head.last_posting - head.first_posting + 1U;
    if (head.posting_count > ordinal_span) {
        return fail_authoritative_adopted(
            error,
            "logical-node record-index authoritative head count exceeds posting range");
    }
    return true;
}

} // namespace

struct LogicalNodeRecordIndexAuthoritativeReader::Impl {
    explicit Impl(std::filesystem::path value)
        : store_root(std::move(value)) {}

    std::filesystem::path store_root;
    LogicalNodeRecordIndexManifest manifest{};
    std::unique_ptr<LogicalNodeRecordIndexReader> storage;
    bool opened{false};
};

LogicalNodeRecordIndexAuthoritativeReader::
LogicalNodeRecordIndexAuthoritativeReader(std::filesystem::path store_root)
    : impl_(std::make_unique<Impl>(std::move(store_root))) {}

LogicalNodeRecordIndexAuthoritativeReader::~LogicalNodeRecordIndexAuthoritativeReader() =
    default;
LogicalNodeRecordIndexAuthoritativeReader::
LogicalNodeRecordIndexAuthoritativeReader(
    LogicalNodeRecordIndexAuthoritativeReader&&) noexcept = default;
LogicalNodeRecordIndexAuthoritativeReader&
LogicalNodeRecordIndexAuthoritativeReader::operator=(
    LogicalNodeRecordIndexAuthoritativeReader&&) noexcept = default;

bool LogicalNodeRecordIndexAuthoritativeReader::open(std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (impl_->opened) {
        return fail_authoritative_adopted(
            error,
            "logical-node record-index authoritative reader is already open");
    }

    impl_->storage =
        std::make_unique<LogicalNodeRecordIndexReader>(impl_->store_root);
    if (!impl_->storage->open(error)) {
        return false;
    }
    impl_->manifest = impl_->storage->manifest();
    impl_->opened = true;
    return true;
}

const LogicalNodeRecordIndexManifest&
LogicalNodeRecordIndexAuthoritativeReader::manifest() const noexcept {
    return impl_->manifest;
}

bool LogicalNodeRecordIndexAuthoritativeReader::read_record(
    std::uint64_t source_record_index,
    std::uint64_t continuation_posting_ordinal,
    std::size_t max_nodes,
    LogicalNodeRecordIndexWindow* result,
    std::string* error) const {
    if (result == nullptr || error == nullptr) {
        return false;
    }
    *result = LogicalNodeRecordIndexWindow{};
    error->clear();
    if (!impl_->opened || impl_->storage == nullptr) {
        return fail_authoritative_adopted(
            error,
            "logical-node record-index authoritative reader is not open");
    }

    LogicalNodeRecordIndexHeadSnapshot head;
    if (!impl_->storage->read_head_snapshot(
            source_record_index,
            &head,
            error) ||
        !validate_authoritative_head(impl_->manifest, head, error)) {
        return false;
    }

    const bool initial =
        continuation_posting_ordinal == kNoLogicalNodeRecordPosting;
    if (head.posting_count == 0U) {
        if (!initial) {
            return fail_authoritative_adopted(
                error,
                "logical-node record-index continuation supplied for empty head");
        }
    } else if (!initial &&
               (continuation_posting_ordinal < head.first_posting ||
                continuation_posting_ordinal > head.last_posting)) {
        return fail_authoritative_adopted(
            error,
            "logical-node record-index continuation is outside authoritative head range");
    }

    LogicalNodeRecordIndexWindow decoded;
    if (!impl_->storage->read_record(
            source_record_index,
            continuation_posting_ordinal,
            max_nodes,
            &decoded,
            error)) {
        return false;
    }

    if (head.posting_count == 0U) {
        if (!decoded.postings.empty() || decoded.truncated ||
            decoded.next_posting_ordinal != kNoLogicalNodeRecordPosting) {
            return fail_authoritative_adopted(
                error,
                "logical-node record-index empty head produced postings");
        }
        *result = std::move(decoded);
        return true;
    }

    const std::uint64_t expected_first =
        initial ? head.first_posting : continuation_posting_ordinal;
    if (decoded.postings.empty() ||
        decoded.postings.front().posting_ordinal != expected_first) {
        return fail_authoritative_adopted(
            error,
            "logical-node record-index authoritative chain start mismatch");
    }

    std::uint64_t previous = kNoLogicalNodeRecordPosting;
    for (const LogicalNodeRecordPosting& posting : decoded.postings) {
        if (posting.posting_ordinal < head.first_posting ||
            posting.posting_ordinal > head.last_posting) {
            return fail_authoritative_adopted(
                error,
                "logical-node record-index posting escapes authoritative head range");
        }
        if (previous != kNoLogicalNodeRecordPosting &&
            posting.posting_ordinal <= previous) {
            return fail_authoritative_adopted(
                error,
                "logical-node record-index authoritative postings are not strictly forward");
        }
        previous = posting.posting_ordinal;
    }

    const std::uint64_t last_emitted = decoded.postings.back().posting_ordinal;
    if (decoded.truncated) {
        if (decoded.next_posting_ordinal == kNoLogicalNodeRecordPosting ||
            decoded.next_posting_ordinal <= last_emitted ||
            decoded.next_posting_ordinal > head.last_posting) {
            return fail_authoritative_adopted(
                error,
                "logical-node record-index truncated continuation escapes authoritative head");
        }
    } else {
        if (decoded.next_posting_ordinal != kNoLogicalNodeRecordPosting ||
            last_emitted != head.last_posting) {
            return fail_authoritative_adopted(
                error,
                "logical-node record-index chain tail disagrees with authoritative head");
        }
    }

    *result = std::move(decoded);
    return true;
}

bool LogicalNodeRecordIndexAuthoritativeReader::node_by_ordinal(
    std::uint64_t ordinal,
    LogicalNodeRecord* node,
    std::string* error) const {
    if (node == nullptr || error == nullptr) {
        return false;
    }
    error->clear();
    if (!impl_->opened || impl_->storage == nullptr) {
        return fail_authoritative_adopted(
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
        return fail_authoritative_adopted(
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
        return fail_authoritative_adopted(
            error,
            "logical-node authoritative same-arena reader is not open");
    }
    return impl_->storage->resolve_semantic(kind, id, value, error);
}

} // namespace zevryon::massivedoc
