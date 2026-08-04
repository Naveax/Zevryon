#include "unicode_stream.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using zevryon::text::DecodedCodePoint;
using zevryon::text::Utf8DecodeError;
using zevryon::text::Utf8ErrorKind;
using zevryon::text::Utf8ErrorPolicy;
using zevryon::text::Utf8StreamDecoder;

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct ShadowAggregate {
    bool any_enabled{false};
    bool all_healthy{true};
    std::uint64_t operations{0};
    std::uint64_t verifications{0};
    std::uint64_t mismatches{0};
    std::string first_mismatch{"none"};
};

struct Summary {
    std::uint64_t semantic_hash{kFnvOffset};
    std::uint64_t logical_bytes{0};
    std::uint64_t rounds{0};
    std::uint64_t feed_calls{0};
    std::uint64_t finish_calls{0};
    std::uint64_t reset_calls{0};
    std::uint64_t decoded_records{0};
    std::uint64_t strict_failures{0};
    std::uint64_t replacement_records{0};
    std::uint64_t malformed_cases{0};
    std::uint64_t discontinuity_cases{0};
    std::uint64_t budget_cases{0};
    std::uint64_t peak_output_records{0};
    ShadowAggregate shadow{};
};

std::uint64_t saturating_add(
    std::uint64_t left,
    std::uint64_t right) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kFnvPrime;
}

void hash_u64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
        hash_byte(
            hash,
            static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void hash_record(
    std::uint64_t& hash,
    const DecodedCodePoint& record) noexcept {
    hash_u64(hash, record.source_start);
    hash_u64(hash, record.value);
    hash_u64(hash, record.source_length);
    hash_u64(hash, record.replacement ? 1U : 0U);
}

std::string json_string_field(
    const std::string& json,
    const std::string& field) {
    const std::string prefix = "\"" + field + "\":\"";
    const std::size_t start = json.find(prefix);
    if (start == std::string::npos) {
        return "none";
    }
    const std::size_t value_start = start + prefix.size();
    const std::size_t end = json.find('"', value_start);
    if (end == std::string::npos) {
        return "none";
    }
    return json.substr(value_start, end - value_start);
}

void merge_shadow(
    Summary& summary,
    const Utf8StreamDecoder& decoder) {
    const bool enabled = decoder.rust_shadow_enabled();
    summary.shadow.any_enabled = summary.shadow.any_enabled || enabled;
    if (enabled) {
        summary.shadow.all_healthy =
            summary.shadow.all_healthy && decoder.rust_shadow_healthy();
    }
    summary.shadow.operations = saturating_add(
        summary.shadow.operations,
        decoder.rust_shadow_operations());
    summary.shadow.verifications = saturating_add(
        summary.shadow.verifications,
        decoder.rust_shadow_verifications());
    summary.shadow.mismatches = saturating_add(
        summary.shadow.mismatches,
        decoder.rust_shadow_mismatches());
    if (summary.shadow.first_mismatch == "none" &&
        decoder.rust_shadow_mismatches() != 0U) {
        summary.shadow.first_mismatch =
            json_string_field(
                decoder.rust_shadow_json(),
                "first_mismatch");
    }
}

void hash_output_and_clear(
    Summary& summary,
    std::pmr::vector<DecodedCodePoint>& output) {
    summary.peak_output_records = std::max(
        summary.peak_output_records,
        static_cast<std::uint64_t>(output.size()));
    for (const DecodedCodePoint& record : output) {
        hash_record(summary.semantic_hash, record);
        ++summary.decoded_records;
        if (record.replacement) {
            ++summary.replacement_records;
        }
    }
    output.clear();
}

std::vector<std::byte> build_valid_corpus(std::size_t logical_bytes) {
    constexpr std::array<std::uint8_t, 29> pattern{
        0x41U,
        0x20U,
        0xc2U, 0xa2U,
        0x20U,
        0xe2U, 0x82U, 0xacU,
        0x20U,
        0xf0U, 0x9fU, 0x98U, 0x80U,
        0x20U,
        0xd8U, 0xa7U,
        0x20U,
        0xe0U, 0xa4U, 0x95U,
        0x20U,
        0xe4U, 0xb8U, 0xadU,
        0x20U,
        0x7aU,
        0x0aU,
        0x31U,
        0x32U,
    };

    std::vector<std::byte> corpus;
    corpus.resize(logical_bytes);
    for (std::size_t index = 0U; index < logical_bytes; ++index) {
        corpus[index] = static_cast<std::byte>(
            pattern[index % pattern.size()]);
    }

    // Do not end inside a multibyte sequence.
    while (!corpus.empty()) {
        const auto byte = static_cast<std::uint8_t>(
            std::to_integer<unsigned int>(corpus.back()));
        if ((byte & 0xc0U) != 0x80U) {
            break;
        }
        corpus.pop_back();
    }
    if (!corpus.empty()) {
        const auto byte = static_cast<std::uint8_t>(
            std::to_integer<unsigned int>(corpus.back()));
        if (byte >= 0xc2U) {
            corpus.pop_back();
        }
    }
    return corpus;
}

bool run_stream(
    Summary& summary,
    Utf8ErrorPolicy policy,
    std::span<const std::byte> bytes,
    std::span<const std::size_t> chunk_pattern,
    bool expect_success,
    Utf8ErrorKind expected_error = Utf8ErrorKind::None) {
    Utf8StreamDecoder decoder(policy);
    std::pmr::vector<DecodedCodePoint> output;
    Utf8DecodeError error{};

    std::size_t cursor = 0U;
    std::size_t chunk_index = 0U;
    bool result = true;
    while (cursor < bytes.size()) {
        const std::size_t requested =
            chunk_pattern[chunk_index % chunk_pattern.size()];
        const std::size_t remaining = bytes.size() - cursor;
        const std::size_t count = std::min(requested, remaining);
        result = decoder.feed(
            bytes.subspan(cursor, count),
            static_cast<std::uint64_t>(cursor),
            &output,
            &error);
        ++summary.feed_calls;
        hash_output_and_clear(summary, output);
        if (!result) {
            break;
        }
        cursor += count;
        ++chunk_index;
    }

    if (result) {
        result = decoder.finish(&output, &error);
        ++summary.finish_calls;
        hash_output_and_clear(summary, output);
    }

    hash_u64(summary.semantic_hash, result ? 1U : 0U);
    hash_u64(
        summary.semantic_hash,
        static_cast<std::uint64_t>(error.kind));
    hash_u64(summary.semantic_hash, error.source_offset);
    const auto& stats = decoder.stats();
    hash_u64(summary.semantic_hash, stats.source_bytes);
    hash_u64(summary.semantic_hash, stats.emitted_codepoints);
    hash_u64(summary.semantic_hash, stats.invalid_sequences);
    hash_u64(summary.semantic_hash, stats.replacements);
    hash_u64(summary.semantic_hash, stats.chunks);
    hash_u64(
        summary.semantic_hash,
        stats.maximum_pending_continuations);
    hash_u64(summary.semantic_hash, decoder.next_source_offset());
    hash_u64(summary.semantic_hash, decoder.failed() ? 1U : 0U);

    bool accepted = result == expect_success;
    if (!expect_success) {
        accepted =
            accepted && error.kind == expected_error && decoder.failed();
        if (accepted) {
            ++summary.strict_failures;
        }
    }

    decoder.reset();
    ++summary.reset_calls;
    hash_u64(summary.semantic_hash, decoder.failed() ? 1U : 0U);
    hash_u64(summary.semantic_hash, decoder.stats().source_bytes);
    merge_shadow(summary, decoder);
    return accepted;
}

bool run_discontinuity_case(Summary& summary) {
    Utf8StreamDecoder decoder(Utf8ErrorPolicy::Strict);
    std::pmr::vector<DecodedCodePoint> output;
    Utf8DecodeError error{};
    const std::array<std::byte, 1> first{std::byte{0x41U}};
    const std::array<std::byte, 1> second{std::byte{0x42U}};

    bool ok = decoder.feed(first, 0U, &output, &error);
    ++summary.feed_calls;
    hash_output_and_clear(summary, output);
    ok = ok && !decoder.feed(second, 2U, &output, &error);
    ++summary.feed_calls;
    hash_output_and_clear(summary, output);
    ok = ok &&
        error.kind == Utf8ErrorKind::DiscontinuousInput &&
        decoder.failed();
    hash_u64(
        summary.semantic_hash,
        static_cast<std::uint64_t>(error.kind));
    hash_u64(summary.semantic_hash, error.source_offset);
    decoder.reset();
    ++summary.reset_calls;
    merge_shadow(summary, decoder);
    ++summary.discontinuity_cases;
    return ok;
}

bool run_budget_case(Summary& summary) {
    Utf8StreamDecoder decoder(Utf8ErrorPolicy::Strict);
    std::array<std::byte, 128> storage{};
    std::pmr::monotonic_buffer_resource arena(
        storage.data(),
        storage.size(),
        std::pmr::null_memory_resource());
    std::pmr::vector<DecodedCodePoint> output(&arena);
    Utf8DecodeError error{};
    std::array<std::byte, 64> input{};
    input.fill(std::byte{0x41U});

    const bool result = decoder.feed(input, 0U, &output, &error);
    ++summary.feed_calls;
    hash_output_and_clear(summary, output);
    const bool ok =
        !result &&
        error.kind == Utf8ErrorKind::OutputBudgetExceeded &&
        decoder.failed();
    hash_u64(
        summary.semantic_hash,
        static_cast<std::uint64_t>(error.kind));
    hash_u64(summary.semantic_hash, error.source_offset);
    decoder.reset();
    ++summary.reset_calls;
    merge_shadow(summary, decoder);
    ++summary.budget_cases;
    return ok;
}

std::uint64_t parse_u64(
    const char* text,
    const char* name) {
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || end == nullptr || *end != '\0') {
        std::cerr << "invalid " << name << ": " << text << '\n';
        std::exit(2);
    }
    return static_cast<std::uint64_t>(value);
}

std::string hex_u64(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t logical_bytes = 16U * 1024U * 1024U;
    std::uint64_t rounds = 2U;

    for (int index = 1; index < argc; ++index) {
        const std::string_view arg(argv[index]);
        if (arg == "--logical-bytes" && index + 1 < argc) {
            logical_bytes = parse_u64(argv[++index], "logical bytes");
        } else if (arg == "--rounds" && index + 1 < argc) {
            rounds = parse_u64(argv[++index], "rounds");
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 2;
        }
    }

    if (logical_bytes < 4096U || logical_bytes > 134217728U) {
        std::cerr << "logical bytes outside certification bounds\n";
        return 2;
    }
    if (rounds == 0U || rounds > 16U) {
        std::cerr << "rounds outside certification bounds\n";
        return 2;
    }

    Summary summary{};
    summary.logical_bytes = logical_bytes;
    summary.rounds = rounds;

    const std::vector<std::byte> corpus =
        build_valid_corpus(static_cast<std::size_t>(logical_bytes));
    constexpr std::array<std::size_t, 8> chunk_pattern{
        1U, 2U, 3U, 7U, 31U, 257U, 4096U, 65536U};

    const auto started = std::chrono::steady_clock::now();
    bool ok = true;
    for (std::uint64_t round = 0U; round < rounds; ++round) {
        ok = ok && run_stream(
            summary,
            Utf8ErrorPolicy::Strict,
            corpus,
            chunk_pattern,
            true);
        ok = ok && run_stream(
            summary,
            Utf8ErrorPolicy::Replace,
            corpus,
            chunk_pattern,
            true);
    }

    const std::array<std::vector<std::byte>, 7> malformed{
        std::vector<std::byte>{std::byte{0xffU}},
        std::vector<std::byte>{std::byte{0x80U}},
        std::vector<std::byte>{
            std::byte{0xe2U}, std::byte{0x28U}, std::byte{0xa1U}},
        std::vector<std::byte>{
            std::byte{0xe0U}, std::byte{0x80U}, std::byte{0x80U}},
        std::vector<std::byte>{
            std::byte{0xedU}, std::byte{0xa0U}, std::byte{0x80U}},
        std::vector<std::byte>{
            std::byte{0xf4U}, std::byte{0x90U},
            std::byte{0x80U}, std::byte{0x80U}},
        std::vector<std::byte>{std::byte{0xe2U}, std::byte{0x82U}},
    };
    constexpr std::array<Utf8ErrorKind, 7> errors{
        Utf8ErrorKind::InvalidLeadByte,
        Utf8ErrorKind::UnexpectedContinuation,
        Utf8ErrorKind::InvalidContinuation,
        Utf8ErrorKind::OverlongEncoding,
        Utf8ErrorKind::SurrogateCodePoint,
        Utf8ErrorKind::CodePointOutOfRange,
        Utf8ErrorKind::TruncatedSequence,
    };
    constexpr std::array<std::size_t, 1> one_byte_chunks{1U};
    for (std::size_t index = 0U; index < malformed.size(); ++index) {
        ok = ok && run_stream(
            summary,
            Utf8ErrorPolicy::Strict,
            malformed[index],
            one_byte_chunks,
            false,
            errors[index]);
        ok = ok && run_stream(
            summary,
            Utf8ErrorPolicy::Replace,
            malformed[index],
            one_byte_chunks,
            true);
        ++summary.malformed_cases;
    }

    ok = ok && run_discontinuity_case(summary);
    ok = ok && run_budget_case(summary);

    const auto finished = std::chrono::steady_clock::now();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(finished - started).count();

    if (!ok) {
        std::cerr << "Unicode promotion-readiness workload failed\n";
        return 1;
    }

    std::cout
        << "{"
        << "\"schema\":\"zevryon.z2r3du.unicode-workload.v1\","
        << "\"logical_bytes\":" << summary.logical_bytes << ','
        << "\"rounds\":" << summary.rounds << ','
        << "\"feed_calls\":" << summary.feed_calls << ','
        << "\"finish_calls\":" << summary.finish_calls << ','
        << "\"reset_calls\":" << summary.reset_calls << ','
        << "\"decoded_records\":" << summary.decoded_records << ','
        << "\"strict_failures\":" << summary.strict_failures << ','
        << "\"replacement_records\":" << summary.replacement_records << ','
        << "\"malformed_cases\":" << summary.malformed_cases << ','
        << "\"discontinuity_cases\":" << summary.discontinuity_cases << ','
        << "\"budget_cases\":" << summary.budget_cases << ','
        << "\"peak_output_records\":" << summary.peak_output_records << ','
        << "\"semantic_checksum\":\""
        << hex_u64(summary.semantic_hash) << "\","
        << "\"elapsed_ms\":" << std::fixed << std::setprecision(6)
        << elapsed_ms << ','
        << "\"shadow\":{"
        << "\"enabled\":"
        << (summary.shadow.any_enabled ? "true" : "false") << ','
        << "\"healthy\":"
        << (summary.shadow.any_enabled && summary.shadow.all_healthy
                ? "true"
                : "false")
        << ','
        << "\"operations\":" << summary.shadow.operations << ','
        << "\"verifications\":" << summary.shadow.verifications << ','
        << "\"mismatches\":" << summary.shadow.mismatches << ','
        << "\"first_mismatch\":\""
        << summary.shadow.first_mismatch << "\""
        << "}"
        << "}\n";
    return 0;
}
