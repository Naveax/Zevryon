#pragma once

#include "order_statistics_sequence.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace zevryon_test {
namespace {

using zevryon::massivedoc::ChunkedOrderStatisticsSequence;
using zevryon::massivedoc::SequenceAggregate;
using zevryon::massivedoc::SequencePosition;
using zevryon::massivedoc::SequenceRecord;

bool sequence_require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: order statistics sequence: " << message << '\n';
        return false;
    }
    return true;
}

bool same_record(const SequenceRecord& lhs, const SequenceRecord& rhs) {
    return lhs.logical_id == rhs.logical_id && lhs.text_bytes == rhs.text_bytes && lhs.height_q8 == rhs.height_q8 &&
           lhs.search_summary == rhs.search_summary;
}

SequenceAggregate oracle_aggregate(const std::vector<SequenceRecord>& records, std::size_t count) {
    SequenceAggregate aggregate;
    count = std::min(count, records.size());
    for (std::size_t index = 0U; index < count; ++index) {
        ++aggregate.record_count;
        aggregate.text_bytes += records[index].text_bytes;
        aggregate.layout_height_q8 += records[index].height_q8;
        aggregate.search_summary |= records[index].search_summary;
    }
    return aggregate;
}

bool verify_sequence(
    ChunkedOrderStatisticsSequence& sequence,
    const std::vector<SequenceRecord>& oracle,
    std::string* error) {
    const SequenceAggregate expected = oracle_aggregate(oracle, oracle.size());
    const SequenceAggregate actual = sequence.stats().aggregate;
    if (!sequence_require(actual.record_count == expected.record_count, "record aggregate") ||
        !sequence_require(actual.text_bytes == expected.text_bytes, "text aggregate") ||
        !sequence_require(actual.layout_height_q8 == expected.layout_height_q8, "height aggregate") ||
        !sequence_require(actual.search_summary == expected.search_summary, "search aggregate")) {
        return false;
    }

    std::uint32_t logarithmic_bound = 2U;
    std::uint64_t chunks = sequence.stats().chunk_count + 1U;
    while (chunks > 1U) {
        chunks = (chunks + 1U) / 2U;
        logarithmic_bound += 2U;
    }
    if (!sequence_require(sequence.stats().tree_height <= logarithmic_bound, "AVL height bound")) {
        return false;
    }

    std::uint64_t text_prefix = 0U;
    std::uint64_t height_prefix = 0U;
    for (std::size_t index = 0U; index < oracle.size(); ++index) {
        SequencePosition position;
        if (!sequence_require(sequence.at(static_cast<std::uint64_t>(index), &position, error), *error) ||
            !sequence_require(position.record_index == index, "record rank") ||
            !sequence_require(position.text_offset == text_prefix, "text prefix") ||
            !sequence_require(position.y_q8 == height_prefix, "height prefix") ||
            !sequence_require(same_record(position.record, oracle[index]), "record identity")) {
            return false;
        }
        text_prefix += oracle[index].text_bytes;
        height_prefix += oracle[index].height_q8;
    }

    for (std::size_t count = 0U; count <= oracle.size(); ++count) {
        SequenceAggregate prefix;
        const SequenceAggregate expected_prefix = oracle_aggregate(oracle, count);
        if (!sequence_require(sequence.prefix(static_cast<std::uint64_t>(count), &prefix, error), *error) ||
            !sequence_require(prefix.record_count == expected_prefix.record_count, "prefix count") ||
            !sequence_require(prefix.text_bytes == expected_prefix.text_bytes, "prefix bytes") ||
            !sequence_require(prefix.layout_height_q8 == expected_prefix.layout_height_q8, "prefix height") ||
            !sequence_require(prefix.search_summary == expected_prefix.search_summary, "prefix search summary")) {
            return false;
        }
    }
    return true;
}

} // namespace

inline bool run_order_statistics_sequence_tests() {
    ChunkedOrderStatisticsSequence sequence(16U);
    std::vector<SequenceRecord> oracle;
    std::string error;

    if (!sequence_require(sequence.empty(), "starts empty") ||
        !sequence_require(!sequence.insert(0U, SequenceRecord{1U, 1U, 0U, 1U}, &error), "zero height rejected")) {
        return false;
    }

    for (std::uint64_t index = 0U; index < 257U; ++index) {
        const SequenceRecord record{
            1000U + index,
            index % 11U,
            static_cast<std::uint32_t>((10U + index % 31U) * 256U),
            1ULL << (index % 63U),
        };
        const std::uint64_t insertion = index % 3U == 0U ? 0U : static_cast<std::uint64_t>(oracle.size());
        if (!sequence_require(sequence.insert(insertion, record, &error), error)) {
            return false;
        }
        oracle.insert(oracle.begin() + static_cast<std::ptrdiff_t>(insertion), record);
    }
    if (!sequence_require(sequence.stats().chunk_count > 1U, "chunk split") ||
        !verify_sequence(sequence, oracle, &error)) {
        return false;
    }

    std::mt19937_64 random(0x4d3253455155454eULL);
    std::uint64_t next_id = 100000U;
    for (std::size_t iteration = 0U; iteration < 5000U; ++iteration) {
        const std::uint64_t action = random() % 5U;
        if (action == 0U || oracle.empty()) {
            const std::size_t index = static_cast<std::size_t>(random() % (oracle.size() + 1U));
            const SequenceRecord record{
                next_id++,
                random() % 4096U,
                static_cast<std::uint32_t>((1U + random() % 1024U) * 256U),
                1ULL << (random() % 63U),
            };
            if (!sequence_require(sequence.insert(static_cast<std::uint64_t>(index), record, &error), error)) {
                return false;
            }
            oracle.insert(oracle.begin() + static_cast<std::ptrdiff_t>(index), record);
        } else if (action == 1U) {
            const std::size_t index = static_cast<std::size_t>(random() % oracle.size());
            SequenceRecord erased;
            if (!sequence_require(sequence.erase(static_cast<std::uint64_t>(index), &erased, &error), error) ||
                !sequence_require(same_record(erased, oracle[index]), "erase identity")) {
                return false;
            }
            oracle.erase(oracle.begin() + static_cast<std::ptrdiff_t>(index));
        } else if (action == 2U && oracle.size() > 1U) {
            const std::size_t from = static_cast<std::size_t>(random() % oracle.size());
            const std::size_t to = static_cast<std::size_t>(random() % oracle.size());
            if (!sequence_require(sequence.move(static_cast<std::uint64_t>(from), static_cast<std::uint64_t>(to), &error), error)) {
                return false;
            }
            const SequenceRecord record = oracle[from];
            oracle.erase(oracle.begin() + static_cast<std::ptrdiff_t>(from));
            oracle.insert(oracle.begin() + static_cast<std::ptrdiff_t>(to), record);
        } else if (action == 3U) {
            const std::size_t index = static_cast<std::size_t>(random() % oracle.size());
            const std::uint32_t height = static_cast<std::uint32_t>((1U + random() % 2048U) * 256U);
            std::uint32_t old_height = 0U;
            if (!sequence_require(sequence.update_height(static_cast<std::uint64_t>(index), height, &old_height, &error), error) ||
                !sequence_require(old_height == oracle[index].height_q8, "height old value")) {
                return false;
            }
            oracle[index].height_q8 = height;
        } else {
            const std::size_t index = static_cast<std::size_t>(random() % oracle.size());
            const std::uint64_t summary = random();
            std::uint64_t old_summary = 0U;
            if (!sequence_require(
                    sequence.update_search_summary(static_cast<std::uint64_t>(index), summary, &old_summary, &error), error) ||
                !sequence_require(old_summary == oracle[index].search_summary, "search old value")) {
                return false;
            }
            oracle[index].search_summary = summary;
        }
        if (iteration % 127U == 0U && !verify_sequence(sequence, oracle, &error)) {
            return false;
        }
    }
    if (!verify_sequence(sequence, oracle, &error)) {
        return false;
    }

    ChunkedOrderStatisticsSequence scale(64U);
    constexpr std::uint64_t kScaleRecords = 100000U;
    for (std::uint64_t index = 0U; index < kScaleRecords; ++index) {
        if (!sequence_require(
                scale.insert(index, SequenceRecord{index, 10U, 256U, 1ULL << (index % 63U)}, &error), error)) {
            return false;
        }
    }
    SequencePosition tail;
    if (!sequence_require(scale.locate_height_offset((kScaleRecords - 1U) * 256U, &tail, &error), error) ||
        !sequence_require(tail.record.logical_id == kScaleRecords - 1U, "100k tail select") ||
        !sequence_require(scale.stats().chunk_count < 4096U, "100k chunk bound") ||
        !sequence_require(scale.stats().tree_height < 32U, "100k tree-height bound")) {
        return false;
    }

    return true;
}

} // namespace zevryon_test
