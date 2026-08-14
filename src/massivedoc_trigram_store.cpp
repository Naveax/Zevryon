#include "massivedoc_trigram_store.hpp"
#include "massivedoc_store.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace zevryon::massivedoc {
namespace {

bool parse_source_identity(
    std::string_view hex,
    std::array<std::uint8_t, kTrigramSourceIdentityBytes>* identity,
    std::string* error) {
    if (hex.size() != identity->size() * 2U) {
        *error = "store payload SHA-256 has an invalid length";
        return false;
    }
    const auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t index = 0U; index < identity->size(); ++index) {
        const int high = nibble(hex[index * 2U]);
        const int low = nibble(hex[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            *error = "store payload SHA-256 is not hexadecimal";
            return false;
        }
        (*identity)[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

} // namespace

bool build_store_trigram_index(
    const std::filesystem::path& root,
    TrigramIndexConfig config,
    const TrigramCancellationCheck& cancelled,
    TrigramIndexStats* stats,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (stats != nullptr) {
        *stats = {};
    }

    StoreReader reader(root);
    if (!reader.open(error)) {
        return false;
    }
    // Candidate data may eliminate work only after the canonical store has passed
    // full payload integrity authority. The source store is immutable after commit.
    if (!reader.verify(error)) {
        return false;
    }

    std::array<std::uint8_t, kTrigramSourceIdentityBytes> identity{};
    if (!parse_source_identity(reader.stats().payload_sha256, &identity, error)) {
        return false;
    }

    TrigramIndexWriter writer(root, config);
    const std::uint64_t records = reader.stats().corpus.logical_records;
    for (std::uint64_t record_index = 0U; record_index < records; ++record_index) {
        if (static_cast<bool>(cancelled) && cancelled()) {
            *error = "trigram index build cancelled";
            return false;
        }
        // Production M4 starts with one immutable source record per candidate block.
        // This keeps exact verification record-local and decouples the derived index
        // from the historical search.bgm record grouping policy.
        if (!writer.begin_record(record_index, error)) {
            return false;
        }
        bool feed_ok = true;
        const bool read_ok = reader.read_record(
            record_index,
            [&](std::span<const std::byte> bytes) {
                if (static_cast<bool>(cancelled) && cancelled()) {
                    *error = "trigram index build cancelled";
                    feed_ok = false;
                    return false;
                }
                if (!writer.feed(bytes, error)) {
                    feed_ok = false;
                    return false;
                }
                return true;
            },
            error);
        if (!read_ok || !feed_ok) {
            return false;
        }
        if (!writer.end_record(error)) {
            return false;
        }
    }

    return writer.finish(records, identity, stats, cancelled, error);
}

} // namespace zevryon::massivedoc
