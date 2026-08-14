#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace zevryon::massivedoc {

struct SequenceRecord {
    std::uint64_t logical_id{0};
    std::uint64_t text_bytes{0};
    std::uint32_t height_q8{0};
    std::uint64_t search_summary{0};
};

struct SequenceAggregate {
    std::uint64_t record_count{0};
    std::uint64_t text_bytes{0};
    std::uint64_t layout_height_q8{0};
    std::uint64_t search_summary{0};
};

struct SequencePosition {
    std::uint64_t record_index{0};
    std::uint64_t text_offset{0};
    std::uint64_t y_q8{0};
    SequenceRecord record{};
};

struct SequenceStats {
    SequenceAggregate aggregate{};
    std::uint64_t chunk_count{0};
    std::uint32_t tree_height{0};
    std::uint32_t max_records_per_chunk{0};
};

class ChunkedOrderStatisticsSequence {
public:
    explicit ChunkedOrderStatisticsSequence(std::uint32_t max_records_per_chunk = 256U);
    ~ChunkedOrderStatisticsSequence();

    ChunkedOrderStatisticsSequence(const ChunkedOrderStatisticsSequence&) = delete;
    ChunkedOrderStatisticsSequence& operator=(const ChunkedOrderStatisticsSequence&) = delete;
    ChunkedOrderStatisticsSequence(ChunkedOrderStatisticsSequence&&) noexcept;
    ChunkedOrderStatisticsSequence& operator=(ChunkedOrderStatisticsSequence&&) noexcept;

    const SequenceStats& stats() const noexcept;
    bool empty() const noexcept;

    bool at(std::uint64_t record_index, SequencePosition* position, std::string* error) const;
    bool locate_text_offset(std::uint64_t text_offset, SequencePosition* position, std::string* error) const;
    bool locate_height_offset(std::uint64_t y_q8, SequencePosition* position, std::string* error) const;
    bool prefix(std::uint64_t record_count, SequenceAggregate* aggregate, std::string* error) const;

    bool insert(std::uint64_t record_index, SequenceRecord record, std::string* error);
    bool erase(std::uint64_t record_index, SequenceRecord* erased, std::string* error);
    bool move(std::uint64_t from_index, std::uint64_t to_index, std::string* error);
    bool update_height(
        std::uint64_t record_index,
        std::uint32_t new_height_q8,
        std::uint32_t* old_height_q8,
        std::string* error);
    bool update_search_summary(
        std::uint64_t record_index,
        std::uint64_t new_summary,
        std::uint64_t* old_summary,
        std::string* error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    SequenceStats stats_{};

    void refresh_stats() noexcept;
};

} // namespace zevryon::massivedoc
