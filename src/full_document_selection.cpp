#include "full_document_selection.hpp"

#include <utility>

namespace zevryon::massivedoc {

FullDocumentSelectionDescriptor full_document_selection(
    ChunkedOrderStatisticsSequence::Snapshot snapshot) noexcept {
    const SequenceAggregate aggregate = snapshot.stats().aggregate;
    return FullDocumentSelectionDescriptor{
        std::move(snapshot),
        0U,
        aggregate.text_bytes,
        aggregate.record_count};
}

} // namespace zevryon::massivedoc
