#include "compact_document.hpp"
#include "massivedoc_store.hpp"
#include "order_statistics_sequence.hpp"
#include "unicode_search_normalization_data.generated.hpp"
#include "unicode_search_normalizer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using zevryon::massivedoc::ArenaConfig;
using zevryon::massivedoc::ArenaStats;
using zevryon::massivedoc::ChunkedOrderStatisticsSequence;
using zevryon::massivedoc::CompactArenaReader;
using zevryon::massivedoc::CorpusMetadata;
using zevryon::massivedoc::HeightUpdateResult;
using zevryon::massivedoc::SearchHit;
using zevryon::massivedoc::SequenceAggregate;
using zevryon::massivedoc::SequencePosition;
using zevryon::massivedoc::SequenceRecord;
using zevryon::massivedoc::StoreConfig;
using zevryon::massivedoc::StoreReader;
using zevryon::massivedoc::StoreStats;
using zevryon::massivedoc::StoreWriter;
using zevryon::text::NormalizedSearchCodePoint;
using zevryon::text::SearchSourceCodePoint;
using zevryon::text::UnicodeSearchNormalizationError;
using zevryon::text::UnicodeSearchNormalizer;
using zevryon::text::UnicodeSearchNormalizerConfig;
using zevryon::text::kUnicodeSearchNormalizationTables;

constexpr std::uint64_t kDefaultSeed = 0x4d385f46555a5aULL;
constexpr std::uint64_t kDefaultSmokeCases = 32ULL;
constexpr std::uint64_t kCertificationCasesPerDomain = 10'000ULL;
constexpr std::size_t kSequenceOperationsPerCase = 96U;

struct Config {
    std::filesystem::path work_dir;
    std::optional<std::filesystem::path> output;
    std::uint64_t cases{kDefaultSmokeCases};
    std::uint64_t seed{kDefaultSeed};
    bool certification{false};
};

struct DomainResult {
    std::string name;
    std::uint64_t cases_completed{0U};
    std::uint64_t failures{0U};
    std::uint64_t digest{0U};
    std::optional<std::uint64_t> failure_case;
    std::uint64_t failure_seed{0U};
    std::string failure_reason;
};

bool parse_u64(std::string_view text, std::uint64_t* value) {
    if (value == nullptr || text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0U;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    *value = parsed;
    return true;
}

bool parse_args(int argc, char** argv, Config* config, std::string* error) {
    if (config == nullptr || error == nullptr) {
        return false;
    }
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg(argv[index]);
        if (arg == "--certification") {
            config->certification = true;
            continue;
        }
        if (arg != "--work-dir" && arg != "--output" && arg != "--cases" && arg != "--seed") {
            *error = "unknown argument: " + std::string(arg);
            return false;
        }
        if (index + 1 >= argc) {
            *error = std::string(arg) + " requires a value";
            return false;
        }
        const std::string_view value(argv[++index]);
        if (arg == "--work-dir") {
            config->work_dir = std::filesystem::path(std::string(value));
        } else if (arg == "--output") {
            config->output = std::filesystem::path(std::string(value));
        } else {
            std::uint64_t parsed = 0U;
            if (!parse_u64(value, &parsed)) {
                *error = std::string(arg) + " must be an unsigned decimal integer";
                return false;
            }
            if (arg == "--cases") {
                config->cases = parsed;
            } else {
                config->seed = parsed;
            }
        }
    }
    if (config->work_dir.empty()) {
        *error = "--work-dir is required";
        return false;
    }
    if (config->cases == 0U) {
        *error = "cases must be positive";
        return false;
    }
    if (config->certification && config->cases < kCertificationCasesPerDomain) {
        *error = "certification mode requires at least 10000 cases per domain";
        return false;
    }
    return true;
}

std::uint64_t splitmix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::uint64_t case_seed(std::uint64_t root, std::uint64_t domain, std::uint64_t index) noexcept {
    return splitmix64(root ^ splitmix64(domain + index * 0x9e3779b97f4a7c15ULL));
}

void digest_mix(std::uint64_t value, std::uint64_t* digest) noexcept {
    constexpr std::uint64_t kOffset = 1469598103934665603ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    if (*digest == 0U) {
        *digest = kOffset;
    }
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        *digest ^= (value >> shift) & 0xffU;
        *digest *= kPrime;
    }
}

std::string hex64(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

std::string json_escape(std::string_view text) {
    std::ostringstream stream;
    for (const unsigned char value : text) {
        switch (value) {
        case '"': stream << "\\\""; break;
        case '\\': stream << "\\\\"; break;
        case '\b': stream << "\\b"; break;
        case '\f': stream << "\\f"; break;
        case '\n': stream << "\\n"; break;
        case '\r': stream << "\\r"; break;
        case '\t': stream << "\\t"; break;
        default:
            if (value < 0x20U) {
                stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned int>(value) << std::dec;
            } else {
                stream << static_cast<char>(value);
            }
            break;
        }
    }
    return stream.str();
}

std::vector<std::byte> bytes_of(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(begin, begin + text.size());
}

void clean_root(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

std::uint32_t random_codepoint(std::mt19937_64* random) {
    switch ((*random)() % 7U) {
    case 0U: return static_cast<std::uint32_t>(0x20U + (*random)() % 0x5fU);
    case 1U: return static_cast<std::uint32_t>(0x00c0U + (*random)() % (0x0250U - 0x00c0U));
    case 2U: return static_cast<std::uint32_t>(0x0300U + (*random)() % 0x70U);
    case 3U: return static_cast<std::uint32_t>(0x0370U + (*random)() % 0x90U);
    case 4U: return static_cast<std::uint32_t>(0xac00U + (*random)() % (0xd7a4U - 0xac00U));
    case 5U: return static_cast<std::uint32_t>(0x4e00U + (*random)() % (0xa000U - 0x4e00U));
    default: return static_cast<std::uint32_t>(0x1f300U + (*random)() % (0x1fb00U - 0x1f300U));
    }
}

std::uint64_t utf8_width(std::uint32_t value) noexcept {
    if (value <= 0x7fU) return 1U;
    if (value <= 0x7ffU) return 2U;
    if (value <= 0xffffU) return 3U;
    return 4U;
}

bool normalize(
    std::span<const SearchSourceCodePoint> source,
    bool chunked,
    std::uint64_t seed,
    std::vector<NormalizedSearchCodePoint>* output,
    std::string* error) {
    output->clear();
    UnicodeSearchNormalizer normalizer(
        kUnicodeSearchNormalizationTables,
        UnicodeSearchNormalizerConfig{1024U, true, true});
    UnicodeSearchNormalizationError normalization_error;
    auto consumer = [&](std::span<const NormalizedSearchCodePoint> values) {
        output->insert(output->end(), values.begin(), values.end());
        return true;
    };
    if (!chunked) {
        if (!normalizer.feed(source, consumer, &normalization_error)) {
            *error = "unicode one-shot feed failed: " + std::string(normalization_error.message);
            return false;
        }
    } else {
        std::mt19937_64 random(seed);
        std::size_t offset = 0U;
        while (offset < source.size()) {
            const std::size_t count = std::min<std::size_t>(1U + random() % 7U, source.size() - offset);
            if (!normalizer.feed(source.subspan(offset, count), consumer, &normalization_error)) {
                *error = "unicode chunked feed failed: " + std::string(normalization_error.message);
                return false;
            }
            offset += count;
        }
    }
    if (!normalizer.finish(consumer, &normalization_error)) {
        *error = "unicode finish failed: " + std::string(normalization_error.message);
        return false;
    }
    return true;
}

bool same_normalized(
    std::span<const NormalizedSearchCodePoint> left,
    std::span<const NormalizedSearchCodePoint> right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (left[index].value != right[index].value ||
            left[index].source_start != right[index].source_start ||
            left[index].source_end != right[index].source_end) {
            return false;
        }
    }
    return true;
}

bool fuzz_unicode_case(std::uint64_t seed, std::uint64_t* digest, std::string* error) {
    std::mt19937_64 random(seed);
    const std::size_t count = 1U + random() % 64U;
    std::vector<SearchSourceCodePoint> source;
    source.reserve(count);
    std::uint64_t offset = 0U;
    for (std::size_t index = 0U; index < count; ++index) {
        const std::uint32_t value = random_codepoint(&random);
        const std::uint64_t width = utf8_width(value);
        source.push_back(SearchSourceCodePoint{value, offset, offset + width});
        offset += width;
    }

    std::vector<NormalizedSearchCodePoint> one_shot;
    std::vector<NormalizedSearchCodePoint> chunked;
    if (!normalize(source, false, seed, &one_shot, error) ||
        !normalize(source, true, seed ^ 0x554e49434f4445ULL, &chunked, error)) {
        return false;
    }
    if (!same_normalized(one_shot, chunked)) {
        *error = "Unicode streaming chunking changed normalized values or source spans";
        return false;
    }

    std::vector<SearchSourceCodePoint> normalized_source;
    normalized_source.reserve(one_shot.size());
    for (const auto& item : one_shot) {
        if (item.value > 0x10ffffU || (item.value >= 0xd800U && item.value <= 0xdfffU) ||
            item.source_start > item.source_end || item.source_end > offset) {
            *error = "Unicode normalizer emitted an invalid scalar/span receipt";
            return false;
        }
        normalized_source.push_back(SearchSourceCodePoint{item.value, item.source_start, item.source_end});
        digest_mix(item.value, digest);
        digest_mix(item.source_start, digest);
        digest_mix(item.source_end, digest);
    }
    std::vector<NormalizedSearchCodePoint> idempotent;
    if (!normalize(normalized_source, false, seed, &idempotent, error)) {
        return false;
    }
    if (idempotent.size() != one_shot.size()) {
        *error = "Unicode normalization was not idempotent in output length";
        return false;
    }
    for (std::size_t index = 0U; index < one_shot.size(); ++index) {
        if (idempotent[index].value != one_shot[index].value) {
            *error = "Unicode normalization was not idempotent in normalized value";
            return false;
        }
    }
    return true;
}

std::string random_ascii(std::mt19937_64* random, std::size_t size) {
    static constexpr std::string_view alphabet =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
    std::string text;
    text.reserve(size);
    for (std::size_t index = 0U; index < size; ++index) {
        text.push_back(alphabet[static_cast<std::size_t>((*random)() % alphabet.size())]);
    }
    return text;
}

bool fuzz_serializer_case(
    const std::filesystem::path& work_dir,
    std::uint64_t case_index,
    std::uint64_t seed,
    std::uint64_t* digest,
    std::string* error) {
    const auto root = work_dir / ("serializer-" + std::to_string(case_index));
    clean_root(root);
    std::mt19937_64 random(seed);
    const std::size_t record_count = 1U + random() % 24U;
    std::vector<std::uint64_t> ids;
    std::vector<std::uint64_t> sizes;
    ids.reserve(record_count);
    sizes.reserve(record_count);
    std::uint64_t total_bytes = 0U;
    std::uint64_t largest = 0U;

    StoreConfig store_config;
    store_config.segment_bytes = 1024U + random() % 7168U;
    store_config.records_per_search_block = static_cast<std::uint32_t>(1U + random() % 8U);
    {
        StoreWriter writer(root, store_config);
        for (std::size_t index = 0U; index < record_count; ++index) {
            const std::size_t size = 1U + random() % 2048U;
            const auto text = random_ascii(&random, size);
            const auto bytes = bytes_of(text);
            const std::uint64_t logical_id = splitmix64(seed + index) | 1U;
            if (!writer.append(logical_id, bytes, error)) {
                *error = "serializer store append failed: " + *error;
                clean_root(root);
                return false;
            }
            ids.push_back(logical_id);
            sizes.push_back(size);
            total_bytes += size;
            largest = std::max<std::uint64_t>(largest, size);
        }
        CorpusMetadata metadata;
        metadata.logical_utf8_bytes = total_bytes;
        metadata.logical_records = record_count;
        metadata.logical_nodes = record_count * 3U + 1U;
        metadata.style_runs = record_count;
        metadata.resource_references = record_count / 3U;
        metadata.largest_record_bytes = largest;
        StoreStats stats;
        if (!writer.finalize(metadata, &stats, error)) {
            *error = "serializer store finalize failed: " + *error;
            clean_root(root);
            return false;
        }
    }

    ArenaConfig arena_config;
    arena_config.records_per_block = static_cast<std::uint32_t>(1U + random() % 16U);
    arena_config.estimated_bytes_per_line = static_cast<std::uint32_t>(8U + random() % 249U);
    arena_config.line_height_q8 = static_cast<std::uint32_t>((8U + random() % 40U) * 256U);
    arena_config.vertical_padding_q8 = static_cast<std::uint32_t>((random() % 17U) * 256U);
    ArenaStats built;
    if (!zevryon::massivedoc::build_compact_arena(root, arena_config, &built, error)) {
        *error = "compact arena serialization failed: " + *error;
        clean_root(root);
        return false;
    }
    if (built.logical_records != record_count || built.config.records_per_block != arena_config.records_per_block ||
        built.config.estimated_bytes_per_line != arena_config.estimated_bytes_per_line ||
        built.config.line_height_q8 != arena_config.line_height_q8 ||
        built.config.vertical_padding_q8 != arena_config.vertical_padding_q8) {
        *error = "compact arena build stats drifted before reopen";
        clean_root(root);
        return false;
    }

    std::uint64_t updated_total = 0U;
    std::uint64_t updated_index = 0U;
    std::uint32_t updated_height = 0U;
    {
        CompactArenaReader reader(root);
        if (!reader.open(error)) {
            *error = "compact arena reopen failed: " + *error;
            clean_root(root);
            return false;
        }
        const auto& reopened = reader.stats();
        if (reopened.logical_records != built.logical_records || reopened.logical_nodes != built.logical_nodes ||
            reopened.total_height_q8 != built.total_height_q8 || reopened.block_count != built.block_count ||
            reopened.physical_bytes != built.physical_bytes ||
            reopened.config.records_per_block != arena_config.records_per_block ||
            reopened.config.estimated_bytes_per_line != arena_config.estimated_bytes_per_line ||
            reopened.config.line_height_q8 != arena_config.line_height_q8 ||
            reopened.config.vertical_padding_q8 != arena_config.vertical_padding_q8) {
            *error = "compact arena serialized header did not roundtrip exactly";
            clean_root(root);
            return false;
        }
        const auto snapshot = reader.logical_snapshot();
        for (std::size_t index = 0U; index < record_count; ++index) {
            SequencePosition position;
            if (!snapshot.at(index, &position, error) || position.record.logical_id != ids[index] ||
                position.record.text_bytes != sizes[index] || position.record.source_record_index != index) {
                if (error->empty()) *error = "compact arena record descriptor roundtrip mismatch";
                clean_root(root);
                return false;
            }
            digest_mix(position.record.logical_id, digest);
            digest_mix(position.record.text_bytes, digest);
            digest_mix(position.record.height_q8, digest);
        }
        updated_index = random() % record_count;
        updated_height = static_cast<std::uint32_t>((1U + random() % 256U) * 256U);
        HeightUpdateResult update;
        if (!reader.update_height(updated_index, updated_height, &update, error)) {
            *error = "compact arena persisted height update failed: " + *error;
            clean_root(root);
            return false;
        }
        updated_total = update.total_height_q8;
    }
    {
        CompactArenaReader reader(root);
        if (!reader.open(error)) {
            *error = "compact arena second reopen failed: " + *error;
            clean_root(root);
            return false;
        }
        SequencePosition position;
        if (reader.stats().total_height_q8 != updated_total ||
            !reader.logical_snapshot().at(updated_index, &position, error) ||
            position.record.height_q8 != updated_height) {
            if (error->empty()) *error = "persisted compact arena mutation did not roundtrip";
            clean_root(root);
            return false;
        }
        digest_mix(reader.stats().total_height_q8, digest);
    }
    clean_root(root);
    return true;
}

std::vector<SearchHit> oracle_hits(
    const std::vector<std::string>& records,
    const std::vector<std::uint64_t>& ids,
    std::string_view query) {
    std::vector<SearchHit> hits;
    for (std::size_t index = 0U; index < records.size(); ++index) {
        const auto offset = records[index].find(query);
        if (offset != std::string::npos) {
            hits.push_back(SearchHit{index, ids[index], offset});
        }
    }
    return hits;
}

bool fuzz_index_case(
    const std::filesystem::path& work_dir,
    std::uint64_t case_index,
    std::uint64_t seed,
    std::uint64_t* digest,
    std::string* error) {
    const auto root = work_dir / ("index-" + std::to_string(case_index));
    clean_root(root);
    std::mt19937_64 random(seed);
    const std::size_t record_count = 4U + random() % 20U;
    std::vector<std::string> records;
    std::vector<std::uint64_t> ids;
    records.reserve(record_count);
    ids.reserve(record_count);

    StoreConfig config;
    config.segment_bytes = 1024U + random() % 3072U;
    config.records_per_search_block = static_cast<std::uint32_t>(1U + random() % 8U);
    {
        StoreWriter writer(root, config);
        std::uint64_t total = 0U;
        std::uint64_t largest = 0U;
        for (std::size_t index = 0U; index < record_count; ++index) {
            const std::size_t size = 32U + random() % 3000U;
            std::string text = random_ascii(&random, size);
            const std::string sentinel = "Q" + std::to_string(case_index) + "R" + std::to_string(index) + "Z";
            const std::size_t insertion = text.size() > sentinel.size() ? random() % (text.size() - sentinel.size() + 1U) : 0U;
            if (insertion + sentinel.size() <= text.size()) {
                text.replace(insertion, sentinel.size(), sentinel);
            }
            const std::uint64_t logical_id = splitmix64(seed ^ index) | 1U;
            const auto bytes = bytes_of(text);
            if (!writer.append(logical_id, bytes, error)) {
                *error = "index store append failed: " + *error;
                clean_root(root);
                return false;
            }
            records.push_back(std::move(text));
            ids.push_back(logical_id);
            total += bytes.size();
            largest = std::max<std::uint64_t>(largest, bytes.size());
        }
        CorpusMetadata metadata;
        metadata.logical_utf8_bytes = total;
        metadata.logical_records = record_count;
        metadata.logical_nodes = record_count;
        metadata.largest_record_bytes = largest;
        StoreStats stats;
        if (!writer.finalize(metadata, &stats, error)) {
            *error = "index store finalize failed: " + *error;
            clean_root(root);
            return false;
        }
    }

    StoreReader reader(root);
    if (!reader.open(error) || !reader.verify(error)) {
        *error = "index store open/verify failed: " + *error;
        clean_root(root);
        return false;
    }
    for (std::size_t query_index = 0U; query_index < 12U; ++query_index) {
        std::string query;
        if (query_index < 8U) {
            const std::size_t record = random() % records.size();
            const std::size_t max_length = std::min<std::size_t>(16U, records[record].size());
            const std::size_t length = std::min<std::size_t>(2U + random() % 15U, max_length);
            const std::size_t start = random() % (records[record].size() - length + 1U);
            query = records[record].substr(start, length);
        } else {
            query = random_ascii(&random, 2U + random() % 12U);
        }
        const auto expected = oracle_hits(records, ids, query);
        error->clear();
        const auto actual = reader.find(query, records.size() + 1U, error);
        if (!error->empty()) {
            *error = "indexed find failed: " + *error;
            clean_root(root);
            return false;
        }
        if (actual.size() != expected.size()) {
            *error = "search index produced a false negative/positive record set";
            clean_root(root);
            return false;
        }
        for (std::size_t index = 0U; index < expected.size(); ++index) {
            if (actual[index].record_index != expected[index].record_index ||
                actual[index].logical_id != expected[index].logical_id ||
                actual[index].byte_offset != expected[index].byte_offset) {
                *error = "search index hit identity/offset disagreed with brute-force oracle";
                clean_root(root);
                return false;
            }
            digest_mix(actual[index].record_index, digest);
            digest_mix(actual[index].logical_id, digest);
            digest_mix(actual[index].byte_offset, digest);
        }
        digest_mix(query.size(), digest);
    }
    clean_root(root);
    return true;
}

bool same_record(const SequenceRecord& left, const SequenceRecord& right) noexcept {
    return left.logical_id == right.logical_id && left.text_bytes == right.text_bytes &&
        left.height_q8 == right.height_q8 && left.search_summary == right.search_summary &&
        left.source_record_index == right.source_record_index;
}

SequenceAggregate oracle_aggregate(std::span<const SequenceRecord> records) {
    SequenceAggregate result;
    for (const auto& record : records) {
        ++result.record_count;
        result.text_bytes += record.text_bytes;
        result.layout_height_q8 += record.height_q8;
        result.search_summary |= record.search_summary;
    }
    return result;
}

template <typename SequenceLike>
bool verify_sequence(
    const SequenceLike& sequence,
    std::span<const SequenceRecord> oracle,
    std::uint64_t* digest,
    std::string* error) {
    const auto aggregate = oracle_aggregate(oracle);
    const auto actual = sequence.stats().aggregate;
    if (actual.record_count != aggregate.record_count || actual.text_bytes != aggregate.text_bytes ||
        actual.layout_height_q8 != aggregate.layout_height_q8 || actual.search_summary != aggregate.search_summary) {
        *error = "sequence aggregate mismatch";
        return false;
    }
    std::uint64_t text_prefix = 0U;
    std::uint64_t height_prefix = 0U;
    for (std::size_t index = 0U; index < oracle.size(); ++index) {
        SequencePosition position;
        if (!sequence.at(index, &position, error) || position.record_index != index ||
            position.text_offset != text_prefix || position.y_q8 != height_prefix ||
            !same_record(position.record, oracle[index])) {
            if (error->empty()) *error = "sequence rank/prefix identity mismatch";
            return false;
        }
        digest_mix(position.record.logical_id, digest);
        digest_mix(position.text_offset, digest);
        digest_mix(position.y_q8, digest);
        text_prefix += oracle[index].text_bytes;
        height_prefix += oracle[index].height_q8;
    }
    const std::array<std::size_t, 3> counts{0U, oracle.size() / 2U, oracle.size()};
    for (const auto count : counts) {
        SequenceAggregate prefix;
        if (!sequence.prefix(count, &prefix, error)) return false;
        const auto expected = oracle_aggregate(oracle.first(count));
        if (prefix.record_count != expected.record_count || prefix.text_bytes != expected.text_bytes ||
            prefix.layout_height_q8 != expected.layout_height_q8 || prefix.search_summary != expected.search_summary) {
            *error = "sequence prefix aggregate mismatch";
            return false;
        }
    }
    if (!oracle.empty()) {
        const std::array<std::uint64_t, 3> text_offsets{0U, aggregate.text_bytes / 2U, aggregate.text_bytes - 1U};
        for (const auto offset : text_offsets) {
            std::uint64_t prefix = 0U;
            std::size_t expected_index = 0U;
            while (expected_index + 1U < oracle.size() && prefix + oracle[expected_index].text_bytes <= offset) {
                prefix += oracle[expected_index].text_bytes;
                ++expected_index;
            }
            SequencePosition position;
            if (!sequence.locate_text_offset(offset, &position, error) || position.record_index != expected_index ||
                position.text_offset != prefix) {
                if (error->empty()) *error = "sequence text-offset selection mismatch";
                return false;
            }
        }
        const std::array<std::uint64_t, 3> height_offsets{0U, aggregate.layout_height_q8 / 2U,
                                                         aggregate.layout_height_q8 - 1U};
        for (const auto offset : height_offsets) {
            std::uint64_t prefix = 0U;
            std::size_t expected_index = 0U;
            while (expected_index + 1U < oracle.size() && prefix + oracle[expected_index].height_q8 <= offset) {
                prefix += oracle[expected_index].height_q8;
                ++expected_index;
            }
            SequencePosition position;
            if (!sequence.locate_height_offset(offset, &position, error) || position.record_index != expected_index ||
                position.y_q8 != prefix) {
                if (error->empty()) *error = "sequence height-offset selection mismatch";
                return false;
            }
        }
    }
    return true;
}

SequenceRecord random_sequence_record(std::mt19937_64* random, std::uint64_t logical_id) {
    return SequenceRecord{
        logical_id,
        1U + (*random)() % 1024U,
        static_cast<std::uint32_t>((1U + (*random)() % 1024U) * 256U),
        (*random)(),
        logical_id ^ 0xa5a5a5a5a5a5a5a5ULL,
    };
}

bool fuzz_sequence_case(std::uint64_t seed, std::uint64_t* digest, std::string* error) {
    std::mt19937_64 random(seed);
    ChunkedOrderStatisticsSequence sequence(static_cast<std::uint32_t>(2U + random() % 31U));
    std::vector<SequenceRecord> oracle;
    const std::size_t initial = 1U + random() % 24U;
    std::uint64_t next_id = 1U;
    for (std::size_t index = 0U; index < initial; ++index) {
        const auto record = random_sequence_record(&random, next_id++);
        if (!sequence.insert(oracle.size(), record, error)) return false;
        oracle.push_back(record);
    }
    const auto frozen = sequence.snapshot();
    const auto frozen_oracle = oracle;

    for (std::size_t operation = 0U; operation < kSequenceOperationsPerCase; ++operation) {
        const std::uint64_t action = random() % 5U;
        if (action == 0U && oracle.size() < 64U) {
            const std::size_t index = random() % (oracle.size() + 1U);
            const auto record = random_sequence_record(&random, next_id++);
            if (!sequence.insert(index, record, error)) return false;
            oracle.insert(oracle.begin() + static_cast<std::ptrdiff_t>(index), record);
        } else if (action == 1U && oracle.size() > 1U) {
            const std::size_t index = random() % oracle.size();
            SequenceRecord erased;
            if (!sequence.erase(index, &erased, error) || !same_record(erased, oracle[index])) {
                if (error->empty()) *error = "sequence erase identity mismatch";
                return false;
            }
            oracle.erase(oracle.begin() + static_cast<std::ptrdiff_t>(index));
        } else if (action == 2U && oracle.size() > 1U) {
            const std::size_t from = random() % oracle.size();
            const std::size_t to = (from + 1U + random() % (oracle.size() - 1U)) % oracle.size();
            if (!sequence.move(from, to, error)) return false;
            const auto record = oracle[from];
            oracle.erase(oracle.begin() + static_cast<std::ptrdiff_t>(from));
            oracle.insert(oracle.begin() + static_cast<std::ptrdiff_t>(to), record);
        } else if (action == 3U) {
            const std::size_t index = random() % oracle.size();
            const std::uint32_t height = static_cast<std::uint32_t>((1U + random() % 2048U) * 256U);
            std::uint32_t old_height = 0U;
            if (!sequence.update_height(index, height, &old_height, error) || old_height != oracle[index].height_q8) {
                if (error->empty()) *error = "sequence height old-value mismatch";
                return false;
            }
            oracle[index].height_q8 = height;
        } else {
            const std::size_t index = random() % oracle.size();
            const std::uint64_t summary = random();
            std::uint64_t old_summary = 0U;
            if (!sequence.update_search_summary(index, summary, &old_summary, error) ||
                old_summary != oracle[index].search_summary) {
                if (error->empty()) *error = "sequence summary old-value mismatch";
                return false;
            }
            oracle[index].search_summary = summary;
        }
        if ((operation + 1U) % 16U == 0U && !verify_sequence(sequence, oracle, digest, error)) {
            return false;
        }
    }
    if (!verify_sequence(sequence, oracle, digest, error) ||
        !verify_sequence(frozen, frozen_oracle, digest, error)) {
        if (error->empty()) *error = "sequence snapshot/live property mismatch";
        return false;
    }
    return true;
}

using DomainCase = bool (*)(std::uint64_t, std::uint64_t*, std::string*);

DomainResult run_simple_domain(
    std::string name,
    std::uint64_t domain_tag,
    const Config& config,
    DomainCase test) {
    DomainResult result;
    result.name = std::move(name);
    for (std::uint64_t index = 0U; index < config.cases; ++index) {
        const std::uint64_t seed = case_seed(config.seed, domain_tag, index);
        std::string error;
        if (!test(seed, &result.digest, &error)) {
            result.failures = 1U;
            result.failure_case = index;
            result.failure_seed = seed;
            result.failure_reason = std::move(error);
            break;
        }
        ++result.cases_completed;
    }
    return result;
}

template <typename Test>
DomainResult run_filesystem_domain(
    std::string name,
    std::uint64_t domain_tag,
    const Config& config,
    Test test) {
    DomainResult result;
    result.name = std::move(name);
    for (std::uint64_t index = 0U; index < config.cases; ++index) {
        const std::uint64_t seed = case_seed(config.seed, domain_tag, index);
        std::string error;
        if (!test(config.work_dir, index, seed, &result.digest, &error)) {
            result.failures = 1U;
            result.failure_case = index;
            result.failure_seed = seed;
            result.failure_reason = std::move(error);
            break;
        }
        ++result.cases_completed;
    }
    return result;
}

std::string domain_json(const DomainResult& result) {
    std::ostringstream stream;
    stream << "{\"name\":\"" << json_escape(result.name) << "\","
           << "\"cases_completed\":" << result.cases_completed << ','
           << "\"failures\":" << result.failures << ','
           << "\"digest\":\"" << hex64(result.digest) << "\","
           << "\"failure_case\":";
    if (result.failure_case) stream << *result.failure_case; else stream << "null";
    stream << ",\"failure_seed\":";
    if (result.failure_case) stream << result.failure_seed; else stream << "null";
    stream << ",\"failure_reason\":";
    if (result.failure_reason.empty()) stream << "null";
    else stream << '"' << json_escape(result.failure_reason) << '"';
    stream << '}';
    return stream.str();
}

std::string report_json(const Config& config, const std::array<DomainResult, 4>& domains) {
    bool all_passed = true;
    for (const auto& domain : domains) {
        all_passed = all_passed && domain.failures == 0U && domain.cases_completed == config.cases;
    }
    const bool threshold_met = config.cases >= kCertificationCasesPerDomain;
    const bool certification_eligible = config.certification && threshold_met && all_passed;
    const bool gate_passed = all_passed && (!config.certification || certification_eligible);
    std::ostringstream stream;
    stream << "{\n"
           << "  \"schema\": \"zevryon.m8.property-fuzz.v1\",\n"
           << "  \"authority\": \"m8-four-domain-property-fuzz-v1\",\n"
           << "  \"mode\": \"" << (config.certification ? "certification" : "smoke") << "\",\n"
           << "  \"seed\": " << config.seed << ",\n"
           << "  \"cases_requested_per_domain\": " << config.cases << ",\n"
           << "  \"certification_minimum_cases_per_domain\": " << kCertificationCasesPerDomain << ",\n"
           << "  \"certification_threshold_met\": " << (threshold_met ? "true" : "false") << ",\n"
           << "  \"certification_eligible\": " << (certification_eligible ? "true" : "false") << ",\n"
           << "  \"domains\": [\n";
    for (std::size_t index = 0U; index < domains.size(); ++index) {
        stream << "    " << domain_json(domains[index]);
        if (index + 1U != domains.size()) stream << ',';
        stream << '\n';
    }
    stream << "  ],\n"
           << "  \"gate_passed\": " << (gate_passed ? "true" : "false") << "\n"
           << "}\n";
    return stream.str();
}

bool write_report(const Config& config, const std::string& report, std::string* error) {
    std::cout << report;
    if (!config.output) return true;
    std::error_code fs_error;
    if (!config.output->parent_path().empty()) {
        std::filesystem::create_directories(config.output->parent_path(), fs_error);
        if (fs_error) {
            *error = "cannot create property-fuzz report directory: " + fs_error.message();
            return false;
        }
    }
    std::ofstream output(*config.output, std::ios::binary | std::ios::trunc);
    if (!output) {
        *error = "cannot create property-fuzz report";
        return false;
    }
    output << report;
    if (!output) {
        *error = "cannot write property-fuzz report";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Config config;
    std::string error;
    if (!parse_args(argc, argv, &config, &error)) {
        std::cerr << "M8 property-fuzz configuration invalid: " << error << '\n';
        return 1;
    }
    clean_root(config.work_dir);
    std::error_code fs_error;
    std::filesystem::create_directories(config.work_dir, fs_error);
    if (fs_error) {
        std::cerr << "M8 property-fuzz work-dir failure: " << fs_error.message() << '\n';
        return 1;
    }
    if (!zevryon::text::validate_unicode_search_normalization_tables(kUnicodeSearchNormalizationTables)) {
        std::cerr << "M8 property-fuzz Unicode production tables are invalid\n";
        return 1;
    }

    std::array<DomainResult, 4> domains{
        run_simple_domain("unicode", 0x554e49434f4445ULL, config, fuzz_unicode_case),
        run_filesystem_domain("serializer", 0x53455249414cULL, config, fuzz_serializer_case),
        run_filesystem_domain("index", 0x494e444558ULL, config, fuzz_index_case),
        run_simple_domain("sequence", 0x53455155454e43ULL, config, fuzz_sequence_case),
    };
    const auto report = report_json(config, domains);
    if (!write_report(config, report, &error)) {
        std::cerr << "M8 property-fuzz report failure: " << error << '\n';
        return 1;
    }
    for (const auto& domain : domains) {
        if (domain.failures != 0U || domain.cases_completed != config.cases) {
            std::cerr << "M8 property-fuzz domain failure: " << domain.name << ": "
                      << domain.failure_reason << '\n';
            return 2;
        }
    }
    return 0;
}
