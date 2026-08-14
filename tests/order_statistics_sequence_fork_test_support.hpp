#pragma once

#include "order_statistics_sequence.hpp"

#include <cstdint>
#include <iostream>
#include <string>

namespace zevryon_test {

inline bool run_order_statistics_sequence_fork_tests() {
    using zevryon::massivedoc::ChunkedOrderStatisticsSequence;
    using zevryon::massivedoc::SequencePosition;
    using zevryon::massivedoc::SequenceRecord;

    const auto require = [](bool condition, const std::string& message) {
        if (!condition) {
            std::cerr << "FAILED: sequence fork: " << message << '\n';
            return false;
        }
        return true;
    };

    ChunkedOrderStatisticsSequence live(8U);
    std::string error;
    if (!require(live.insert(0U, SequenceRecord{11U, 101U, 256U, 0x1U, 7001U}, &error), error) ||
        !require(live.insert(1U, SequenceRecord{12U, 102U, 512U, 0x2U, 7002U}, &error), error) ||
        !require(live.insert(2U, SequenceRecord{13U, 103U, 768U, 0x4U, 7003U}, &error), error)) {
        return false;
    }

    const auto frozen = live.snapshot();
    auto candidate = live.fork_shared_root();
    if (!require(candidate.stats().aggregate.record_count == live.stats().aggregate.record_count,
                 "fork shares equivalent aggregate") ||
        !require(candidate.move(0U, 2U, &error), error)) {
        return false;
    }

    SequencePosition live_first;
    SequencePosition candidate_last;
    SequencePosition frozen_first;
    if (!require(live.at(0U, &live_first, &error), error) ||
        !require(candidate.at(2U, &candidate_last, &error), error) ||
        !require(frozen.at(0U, &frozen_first, &error), error) ||
        !require(live_first.record.logical_id == 11U,
                 "candidate move does not mutate live root") ||
        !require(candidate_last.record.logical_id == 11U,
                 "candidate receives moved logical identity") ||
        !require(candidate_last.record.source_record_index == 7001U,
                 "candidate move preserves physical source locator") ||
        !require(frozen_first.record.logical_id == 11U,
                 "pre-fork snapshot remains immutable")) {
        return false;
    }

    std::uint32_t old_height = 0U;
    if (!require(candidate.update_height(2U, 1024U, &old_height, &error), error) ||
        !require(old_height == 256U, "candidate reports original height")) {
        return false;
    }
    SequencePosition live_after_height;
    SequencePosition candidate_after_height;
    if (!require(live.at(0U, &live_after_height, &error), error) ||
        !require(candidate.at(2U, &candidate_after_height, &error), error) ||
        !require(live_after_height.record.height_q8 == 256U,
                 "candidate height update does not mutate live root") ||
        !require(candidate_after_height.record.height_q8 == 1024U,
                 "candidate publishes independent COW height root")) {
        return false;
    }

    return true;
}

} // namespace zevryon_test
