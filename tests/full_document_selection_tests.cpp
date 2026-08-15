#include "full_document_selection.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {
using namespace zevryon::massivedoc;

[[noreturn]] void die(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool value, std::string_view message) {
    if (!value) {
        die(message);
    }
}

SequenceRecord record(
    std::uint64_t logical_id,
    std::uint64_t text_bytes,
    std::uint64_t source_record_index) {
    return SequenceRecord{logical_id, text_bytes, 256U, 0U, source_record_index};
}

void test_empty_descriptor() {
    ChunkedOrderStatisticsSequence sequence;
    const auto descriptor = full_document_selection(sequence.snapshot());
    require(descriptor.empty(), "empty descriptor must be empty");
    require(descriptor.record_count == 0U, "empty record count");
    require(descriptor.start_text_offset == 0U, "empty start");
    require(descriptor.end_text_offset == 0U, "empty end");
    require(descriptor.text_bytes() == 0U, "empty byte span");
}

void test_root_aggregate_descriptor() {
    ChunkedOrderStatisticsSequence sequence(2U);
    std::string error;
    require(sequence.insert(0U, record(10U, 5U, 100U), &error), "insert record 10");
    require(sequence.insert(1U, record(20U, 7U, 200U), &error), "insert record 20");
    require(sequence.insert(2U, record(30U, 11U, 300U), &error), "insert record 30");

    const auto descriptor = full_document_selection(sequence.snapshot());
    require(!descriptor.empty(), "descriptor unexpectedly empty");
    require(descriptor.record_count == 3U, "descriptor record count");
    require(descriptor.start_text_offset == 0U, "descriptor start");
    require(descriptor.end_text_offset == 23U, "descriptor end");
    require(descriptor.text_bytes() == 23U, "descriptor text bytes");
    require(
        descriptor.snapshot.stats().aggregate.text_bytes == descriptor.end_text_offset,
        "descriptor must equal snapshot root aggregate");
}

void test_descriptor_owns_immutable_snapshot() {
    ChunkedOrderStatisticsSequence sequence(2U);
    std::string error;
    require(sequence.insert(0U, record(10U, 5U, 100U), &error), "insert record 10");
    require(sequence.insert(1U, record(20U, 7U, 200U), &error), "insert record 20");
    require(sequence.insert(2U, record(30U, 11U, 300U), &error), "insert record 30");

    const auto descriptor = full_document_selection(sequence.snapshot());

    require(sequence.move(2U, 0U, &error), "move live record");
    require(sequence.insert(1U, record(40U, 13U, 400U), &error), "insert live record");
    SequenceRecord erased;
    require(sequence.erase(3U, &erased, &error), "erase live record");

    require(descriptor.record_count == 3U, "snapshot descriptor record count changed");
    require(descriptor.text_bytes() == 23U, "snapshot descriptor text span changed");

    SequencePosition first;
    SequencePosition second;
    SequencePosition third;
    require(descriptor.snapshot.at(0U, &first, &error), "snapshot first record");
    require(descriptor.snapshot.at(1U, &second, &error), "snapshot second record");
    require(descriptor.snapshot.at(2U, &third, &error), "snapshot third record");
    require(first.record.logical_id == 10U, "snapshot first logical id changed");
    require(second.record.logical_id == 20U, "snapshot second logical id changed");
    require(third.record.logical_id == 30U, "snapshot third logical id changed");

    const auto live_descriptor = full_document_selection(sequence.snapshot());
    require(live_descriptor.record_count == 3U, "live descriptor record count");
    require(live_descriptor.text_bytes() == 29U, "live descriptor aggregate bytes");
}

} // namespace

int main() {
    test_empty_descriptor();
    test_root_aggregate_descriptor();
    test_descriptor_owns_immutable_snapshot();
    std::cout << "Zevryon full-document selection descriptor tests passed\n";
    return 0;
}
