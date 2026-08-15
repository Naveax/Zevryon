#pragma once

#include "order_statistics_sequence.hpp"

#include <cstdint>

namespace zevryon::massivedoc {

struct FullDocumentSelectionDescriptor {
    ChunkedOrderStatisticsSequence::Snapshot snapshot{};
    std::uint64_t start_text_offset{0U};
    std::uint64_t end_text_offset{0U};
    std::uint64_t record_count{0U};

    bool empty() const noexcept {
        return record_count == 0U;
    }

    std::uint64_t text_bytes() const noexcept {
        return end_text_offset - start_text_offset;
    }
};

FullDocumentSelectionDescriptor full_document_selection(
    ChunkedOrderStatisticsSequence::Snapshot snapshot) noexcept;

} // namespace zevryon::massivedoc
