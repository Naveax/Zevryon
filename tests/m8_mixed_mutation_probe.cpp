#include "order_statistics_sequence.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using zevryon::massivedoc::ChunkedOrderStatisticsSequence;
using zevryon::massivedoc::SequenceAggregate;
using zevryon::massivedoc::SequencePosition;
using zevryon::massivedoc::SequenceRecord;

constexpr std::uint64_t kCertificationOperations = 10'000'000ULL;
constexpr std::uint64_t kDefaultSmokeOperations = 50'000ULL;
constexpr std::uint64_t kDefaultSeed = 0x4d385f4d55544154ULL;
constexpr std::uint64_t kCheckpointInterval = 4096ULL;
constexpr std::size_t kInitialRecords = 96U;
constexpr std::size_t kMinimumRecords = 32U;
constexpr std::size_t kMaximumRecords = 128U;

struct Config {
    std::uint64_t operations{kDefaultSmokeOperations};
    std::uint64_t seed{kDefaultSeed};
    bool certification{false};
    std::optional<std::filesystem::path> output;
};

struct OperationCounts {
    std::uint64_t insert{0};
    std::uint64_t erase{0};
    std::uint64_t move{0};
    std::uint64_t update_height{0};
    std::uint64_t update_summary{0};
};

struct VerificationReceipt {
    std::uint64_t checkpoints{0};
    std::uint64_t live_digest{0};
    std::uint64_t oracle_digest{0};
};

bool parse_u64(std::string_view text, std::uint64_t* value) {
    if (value == nullptr || text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0U;
    for (char character : text) {
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
        if (arg == "--operations" || arg == "--seed" || arg == "--output") {
            if (index + 1 >= argc) {
                *error = std::string(arg) + " requires a value";
                return false;
            }
            const std::string_view value(argv[++index]);
            if (arg == "--output") {
                config->output = std::filesystem::path(std::string(value));
                continue;
            }
            std::uint64_t parsed = 0U;
            if (!parse_u64(value, &parsed)) {
                *error = std::string(arg) + " must be an unsigned decimal integer";
                return false;
            }
            if (arg == "--operations") {
                config->operations = parsed;
            } else {
                config->seed = parsed;
            }
            continue;
        }
        *error = "unknown argument: " + std::string(arg);
        return false;
    }
    if (config->operations == 0U) {
        *error = "operations must be positive";
        return false;
    }
    if (config->certification && config->operations < kCertificationOperations) {
        *error = "certification mode requires at least 10000000 operations";
        return false;
    }
    return true;
}

bool same_record(const SequenceRecord& left, const SequenceRecord& right) {
    return left.logical_id == right.logical_id &&
           left.text_bytes == right.text_bytes &&
           left.height_q8 == right.height_q8 &&
           left.search_summary == right.search_summary &&
           left.source_record_index == right.source_record_index;
}

SequenceAggregate oracle_aggregate(const std::vector<SequenceRecord>& records) {
    SequenceAggregate aggregate;
    for (const auto& record : records) {
        ++aggregate.record_count;
        aggregate.text_bytes += record.text_bytes;
        aggregate.layout_height_q8 += record.height_q8;
        aggregate.search_summary |= record.search_summary;
    }
    return aggregate;
}

void digest_mix(std::uint64_t value, std::uint64_t* digest) {
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

std::uint64_t digest_records(const std::vector<SequenceRecord>& records) {
    std::uint64_t digest = 0U;
    digest_mix(static_cast<std::uint64_t>(records.size()), &digest);
    for (const auto& record : records) {
        digest_mix(record.logical_id, &digest);
        digest_mix(record.text_bytes, &digest);
        digest_mix(record.height_q8, &digest);
        digest_mix(record.search_summary, &digest);
        digest_mix(record.source_record_index, &digest);
    }
    return digest;
}

template <typename SequenceLike>
bool verify_sequence(
    const SequenceLike& sequence,
    const std::vector<SequenceRecord>& oracle,
    std::uint64_t* digest,
    std::string* error) {
    if (digest == nullptr || error == nullptr) {
        return false;
    }
    const auto expected = oracle_aggregate(oracle);
    const auto actual = sequence.stats().aggregate;
    if (actual.record_count != expected.record_count ||
        actual.text_bytes != expected.text_bytes ||
        actual.layout_height_q8 != expected.layout_height_q8 ||
        actual.search_summary != expected.search_summary) {
        *error = "aggregate mismatch";
        return false;
    }

    std::vector<SequenceRecord> materialized;
    materialized.reserve(oracle.size());
    std::uint64_t text_prefix = 0U;
    std::uint64_t height_prefix = 0U;
    for (std::size_t index = 0U; index < oracle.size(); ++index) {
        SequencePosition position;
        if (!sequence.at(static_cast<std::uint64_t>(index), &position, error)) {
            return false;
        }
        if (position.record_index != index ||
            position.text_offset != text_prefix ||
            position.y_q8 != height_prefix ||
            !same_record(position.record, oracle[index])) {
            *error = "logical order or prefix mismatch at index " + std::to_string(index);
            return false;
        }
        materialized.push_back(position.record);
        text_prefix += oracle[index].text_bytes;
        height_prefix += oracle[index].height_q8;
    }

    const std::array<std::size_t, 3> prefix_counts{
        0U,
        oracle.size() / 2U,
        oracle.size(),
    };
    for (const auto count : prefix_counts) {
        zevryon::massivedoc::SequenceAggregate prefix;
        if (!sequence.prefix(static_cast<std::uint64_t>(count), &prefix, error)) {
            return false;
        }
        SequenceAggregate expected_prefix;
        for (std::size_t index = 0U; index < count; ++index) {
            ++expected_prefix.record_count;
            expected_prefix.text_bytes += oracle[index].text_bytes;
            expected_prefix.layout_height_q8 += oracle[index].height_q8;
            expected_prefix.search_summary |= oracle[index].search_summary;
        }
        if (prefix.record_count != expected_prefix.record_count ||
            prefix.text_bytes != expected_prefix.text_bytes ||
            prefix.layout_height_q8 != expected_prefix.layout_height_q8 ||
            prefix.search_summary != expected_prefix.search_summary) {
            *error = "prefix aggregate mismatch";
            return false;
        }
    }

    *digest = digest_records(materialized);
    const auto expected_digest = digest_records(oracle);
    if (*digest != expected_digest) {
        *error = "logical order digest mismatch";
        return false;
    }
    return true;
}

SequenceRecord random_record(std::mt19937_64* random, std::uint64_t logical_id) {
    return SequenceRecord{
        logical_id,
        (*random)() % 4096U,
        static_cast<std::uint32_t>((1U + (*random)() % 2048U) * 256U),
        (*random)(),
        logical_id,
    };
}

std::string hex64(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

std::string report_json(
    const Config& config,
    const OperationCounts& counts,
    const VerificationReceipt& receipt,
    const ChunkedOrderStatisticsSequence& sequence,
    bool passed) {
    const auto& aggregate = sequence.stats().aggregate;
    std::ostringstream stream;
    stream << "{\n"
           << "  \"schema\": \"zevryon.m8.mixed-mutation.v1\",\n"
           << "  \"authority\": \"m8-sequence-mixed-mutation-integrity-v1\",\n"
           << "  \"mode\": \"" << (config.certification ? "certification" : "smoke") << "\",\n"
           << "  \"operations_requested\": " << config.operations << ",\n"
           << "  \"operations_completed\": " << config.operations << ",\n"
           << "  \"certification_minimum_operations\": " << kCertificationOperations << ",\n"
           << "  \"certification_eligible\": "
           << (config.certification && config.operations >= kCertificationOperations ? "true" : "false") << ",\n"
           << "  \"seed\": " << config.seed << ",\n"
           << "  \"operation_counts\": {\n"
           << "    \"insert\": " << counts.insert << ",\n"
           << "    \"erase\": " << counts.erase << ",\n"
           << "    \"move\": " << counts.move << ",\n"
           << "    \"update_height\": " << counts.update_height << ",\n"
           << "    \"update_summary\": " << counts.update_summary << "\n"
           << "  },\n"
           << "  \"verification_checkpoints\": " << receipt.checkpoints << ",\n"
           << "  \"live_logical_order_digest\": \"" << hex64(receipt.live_digest) << "\",\n"
           << "  \"oracle_logical_order_digest\": \"" << hex64(receipt.oracle_digest) << "\",\n"
           << "  \"final_record_count\": " << aggregate.record_count << ",\n"
           << "  \"final_text_bytes\": " << aggregate.text_bytes << ",\n"
           << "  \"final_layout_height_q8\": " << aggregate.layout_height_q8 << ",\n"
           << "  \"final_search_summary\": " << aggregate.search_summary << ",\n"
           << "  \"logical_order_mismatches\": 0,\n"
           << "  \"integrity_mismatches\": 0,\n"
           << "  \"gate_passed\": " << (passed ? "true" : "false") << "\n"
           << "}\n";
    return stream.str();
}

bool write_report(const Config& config, const std::string& text, std::string* error) {
    std::cout << text;
    if (!config.output) {
        return true;
    }
    std::error_code fs_error;
    if (!config.output->parent_path().empty()) {
        std::filesystem::create_directories(config.output->parent_path(), fs_error);
        if (fs_error) {
            *error = "cannot create report directory: " + fs_error.message();
            return false;
        }
    }
    std::ofstream output(*config.output, std::ios::binary | std::ios::trunc);
    if (!output) {
        *error = "cannot create report output";
        return false;
    }
    output << text;
    if (!output) {
        *error = "cannot write report output";
        return false;
    }
    return true;
}

bool run_mutations(
    const Config& config,
    OperationCounts* counts,
    VerificationReceipt* receipt,
    ChunkedOrderStatisticsSequence* sequence,
    std::string* error) {
    if (counts == nullptr || receipt == nullptr || sequence == nullptr || error == nullptr) {
        return false;
    }

    std::mt19937_64 random(config.seed);
    std::vector<SequenceRecord> oracle;
    oracle.reserve(kMaximumRecords + 1U);
    std::uint64_t next_id = 1U;
    for (std::size_t index = 0U; index < kInitialRecords; ++index) {
        const auto record = random_record(&random, next_id++);
        if (!sequence->insert(static_cast<std::uint64_t>(oracle.size()), record, error)) {
            return false;
        }
        oracle.push_back(record);
    }

    std::optional<ChunkedOrderStatisticsSequence::Snapshot> previous_snapshot;
    std::vector<SequenceRecord> previous_oracle;

    for (std::uint64_t iteration = 0U; iteration < config.operations; ++iteration) {
        std::uint64_t action = random() % 10U;
        if (action == 0U && oracle.size() < kMaximumRecords) {
            const std::size_t index = static_cast<std::size_t>(random() % (oracle.size() + 1U));
            const auto record = random_record(&random, next_id++);
            error->clear();
            if (!sequence->insert(static_cast<std::uint64_t>(index), record, error)) {
                return false;
            }
            oracle.insert(oracle.begin() + static_cast<std::ptrdiff_t>(index), record);
            ++counts->insert;
        } else if (action == 1U && oracle.size() > kMinimumRecords) {
            const std::size_t index = static_cast<std::size_t>(random() % oracle.size());
            SequenceRecord erased;
            error->clear();
            if (!sequence->erase(static_cast<std::uint64_t>(index), &erased, error)) {
                return false;
            }
            if (!same_record(erased, oracle[index])) {
                *error = "erase identity mismatch";
                return false;
            }
            oracle.erase(oracle.begin() + static_cast<std::ptrdiff_t>(index));
            ++counts->erase;
        } else if ((action == 2U || action == 3U) && oracle.size() > 1U) {
            const std::size_t from = static_cast<std::size_t>(random() % oracle.size());
            const std::size_t to = static_cast<std::size_t>(
                (from + 1U + random() % (oracle.size() - 1U)) % oracle.size());
            error->clear();
            if (!sequence->move(static_cast<std::uint64_t>(from), static_cast<std::uint64_t>(to), error)) {
                return false;
            }
            const auto record = oracle[from];
            oracle.erase(oracle.begin() + static_cast<std::ptrdiff_t>(from));
            oracle.insert(oracle.begin() + static_cast<std::ptrdiff_t>(to), record);
            ++counts->move;
        } else if (action <= 6U) {
            const std::size_t index = static_cast<std::size_t>(random() % oracle.size());
            const std::uint32_t height = static_cast<std::uint32_t>((1U + random() % 4096U) * 256U);
            std::uint32_t old_height = 0U;
            error->clear();
            if (!sequence->update_height(static_cast<std::uint64_t>(index), height, &old_height, error)) {
                return false;
            }
            if (old_height != oracle[index].height_q8) {
                *error = "height old-value mismatch";
                return false;
            }
            oracle[index].height_q8 = height;
            ++counts->update_height;
        } else {
            const std::size_t index = static_cast<std::size_t>(random() % oracle.size());
            const std::uint64_t summary = random();
            std::uint64_t old_summary = 0U;
            error->clear();
            if (!sequence->update_search_summary(static_cast<std::uint64_t>(index), summary, &old_summary, error)) {
                return false;
            }
            if (old_summary != oracle[index].search_summary) {
                *error = "search-summary old-value mismatch";
                return false;
            }
            oracle[index].search_summary = summary;
            ++counts->update_summary;
        }

        if ((iteration + 1U) % kCheckpointInterval == 0U) {
            if (previous_snapshot) {
                std::uint64_t snapshot_digest = 0U;
                if (!verify_sequence(*previous_snapshot, previous_oracle, &snapshot_digest, error)) {
                    *error = "snapshot verification failed: " + *error;
                    return false;
                }
            }
            std::uint64_t live_digest = 0U;
            if (!verify_sequence(*sequence, oracle, &live_digest, error)) {
                *error = "live checkpoint verification failed: " + *error;
                return false;
            }
            previous_snapshot = sequence->snapshot();
            previous_oracle = oracle;
            ++receipt->checkpoints;
        }
    }

    if (previous_snapshot) {
        std::uint64_t snapshot_digest = 0U;
        if (!verify_sequence(*previous_snapshot, previous_oracle, &snapshot_digest, error)) {
            *error = "final snapshot verification failed: " + *error;
            return false;
        }
    }

    if (!verify_sequence(*sequence, oracle, &receipt->live_digest, error)) {
        *error = "final live verification failed: " + *error;
        return false;
    }
    receipt->oracle_digest = digest_records(oracle);
    if (receipt->live_digest != receipt->oracle_digest) {
        *error = "final digest mismatch";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Config config;
    std::string error;
    if (!parse_args(argc, argv, &config, &error)) {
        std::cerr << "M8 mixed-mutation configuration invalid: " << error << '\n';
        return 1;
    }

    ChunkedOrderStatisticsSequence sequence(16U);
    OperationCounts counts;
    VerificationReceipt receipt;
    if (!run_mutations(config, &counts, &receipt, &sequence, &error)) {
        std::cerr << "M8 mixed-mutation integrity failure: " << error << '\n';
        return 2;
    }

    const auto report = report_json(config, counts, receipt, sequence, true);
    if (!write_report(config, report, &error)) {
        std::cerr << "M8 mixed-mutation report failure: " << error << '\n';
        return 1;
    }
    return 0;
}
