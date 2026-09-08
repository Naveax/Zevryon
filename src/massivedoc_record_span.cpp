#include "massivedoc_store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace zevryon::massivedoc {

bool StoreReader::read_record_span(
    std::uint64_t start_record_index,
    std::uint64_t start_byte_offset,
    std::uint64_t byte_length,
    const std::function<bool(std::span<const std::byte>)>& consumer,
    std::string* error) const {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (!consumer) {
        *error = "record span consumer is empty";
        return false;
    }
    if (start_record_index >= stats().corpus.logical_records) {
        *error = "record span start record is out of range";
        return false;
    }

    if (byte_length == 0U) {
        // Reuse the descriptor-backed slice validator without materializing
        // payload bytes. This permits the canonical end-of-record position.
        std::vector<std::byte> probe;
        if (!read_record_slice(
                start_record_index,
                start_byte_offset,
                0U,
                &probe,
                error)) {
            *error = "record span start position is invalid: " + *error;
            return false;
        }
        return true;
    }

    std::uint64_t remaining = byte_length;
    std::uint64_t record_index = start_record_index;
    std::uint64_t skip = start_byte_offset;

    while (remaining != 0U) {
        if (record_index >= stats().corpus.logical_records) {
            *error = "record span escapes the authoritative record sequence";
            return false;
        }

        std::uint64_t record_position = 0U;
        bool consumer_stopped = false;
        const bool first_record = record_index == start_record_index;
        if (!read_record(
                record_index,
                [&](std::span<const std::byte> chunk) {
                    const std::uint64_t chunk_bytes =
                        static_cast<std::uint64_t>(chunk.size());
                    if (record_position >
                        std::numeric_limits<std::uint64_t>::max() - chunk_bytes) {
                        if (error != nullptr) {
                            *error = "record span record-position overflow";
                        }
                        consumer_stopped = true;
                        return false;
                    }
                    const std::uint64_t chunk_end = record_position + chunk_bytes;
                    if (skip >= chunk_end) {
                        record_position = chunk_end;
                        return true;
                    }

                    const std::uint64_t begin64 =
                        skip > record_position ? skip - record_position : 0U;
                    const std::size_t begin = static_cast<std::size_t>(begin64);
                    const std::size_t available = chunk.size() - begin;
                    const std::size_t amount = static_cast<std::size_t>(
                        std::min<std::uint64_t>(remaining, available));
                    if (amount != 0U) {
                        if (!consumer(chunk.subspan(begin, amount))) {
                            consumer_stopped = true;
                            return false;
                        }
                        remaining -= static_cast<std::uint64_t>(amount);
                    }
                    record_position = chunk_end;
                    return remaining != 0U;
                },
                error)) {
            return false;
        }

        if (consumer_stopped) {
            // A real internal overflow writes an error before stopping. An
            // ordinary consumer false is successful early termination.
            return error->empty();
        }
        if (first_record && record_position < skip) {
            *error = "record span start byte offset is out of range";
            return false;
        }
        if (remaining == 0U) {
            return true;
        }

        skip = 0U;
        if (record_index == std::numeric_limits<std::uint64_t>::max()) {
            *error = "record span record index overflows";
            return false;
        }
        ++record_index;
    }
    return true;
}

} // namespace zevryon::massivedoc
