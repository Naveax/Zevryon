#include "ledger_memory_resource.hpp"
#include "unicode_stream.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace {

bool parse_size(const char* text, std::size_t* value) {
    if (text == nullptr || value == nullptr) {
        return false;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' ||
        parsed > static_cast<unsigned long long>(
                     std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    *value = static_cast<std::size_t>(parsed);
    return true;
}

std::vector<std::byte> make_fixture(std::size_t target_bytes) {
    const std::vector<std::uint8_t> pattern{
        0x5aU,
        0x65U, 0x76U, 0x72U, 0x79U, 0x6fU, 0x6eU, 0x20U,
        0xc5U, 0x9fU, 0xc4U, 0x9fU, 0xc4U, 0xb1U, 0x20U,
        0x65U, 0xccU, 0x81U, 0x20U,
        0xe4U, 0xb8U, 0xadU, 0x20U,
        0xd7U, 0xa9U, 0x20U,
        0xd8U, 0xb9U, 0x20U,
        0xf0U, 0x9fU, 0x98U, 0x80U, 0x0aU,
    };
    std::vector<std::byte> fixture;
    fixture.reserve(target_bytes);
    while (fixture.size() < target_bytes) {
        const std::size_t remaining = target_bytes - fixture.size();
        const std::size_t count = std::min(remaining, pattern.size());
        for (std::size_t index = 0U; index < count; ++index) {
            fixture.push_back(static_cast<std::byte>(pattern[index]));
        }
    }

    // The repeating pattern may be cut inside a UTF-8 sequence. Replace trailing
    // continuation bytes and an incomplete lead with ASCII so the fixed-size
    // benchmark corpus is always valid without changing its byte length.
    std::size_t suffix = fixture.size();
    while (suffix > 0U) {
        const std::uint8_t byte = static_cast<std::uint8_t>(
            std::to_integer<unsigned int>(fixture[suffix - 1U]));
        if ((byte & 0xc0U) != 0x80U) {
            break;
        }
        fixture[suffix - 1U] = static_cast<std::byte>('x');
        --suffix;
    }
    if (suffix > 0U) {
        const std::uint8_t byte = static_cast<std::uint8_t>(
            std::to_integer<unsigned int>(fixture[suffix - 1U]));
        if (byte >= 0xc2U) {
            fixture[suffix - 1U] = static_cast<std::byte>('x');
        }
    }
    return fixture;
}

double percentile(std::vector<double> values, double percentage) {
    std::sort(values.begin(), values.end());
    const double position =
        static_cast<double>(values.size() - 1U) * percentage / 100.0;
    const std::size_t lower = static_cast<std::size_t>(position);
    const std::size_t upper = std::min(lower + 1U, values.size() - 1U);
    const double fraction = position - static_cast<double>(lower);
    return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

} // namespace

int main(int argc, char** argv) {
    std::size_t iterations = 1024U;
    std::size_t chunk_bytes = 4096U;
    std::size_t unicode_budget_bytes = 2U * 1024U * 1024U;
    if ((argc > 1 && !parse_size(argv[1], &iterations)) ||
        (argc > 2 && !parse_size(argv[2], &chunk_bytes)) ||
        (argc > 3 && !parse_size(argv[3], &unicode_budget_bytes)) ||
        argc > 4 || iterations < 10U || chunk_bytes == 0U ||
        unicode_budget_bytes == 0U) {
        std::cerr << "usage: zevryon-unicode-benchmark [iterations>=10] "
                     "[chunk-bytes] [unicode-budget-bytes]\n";
        return 2;
    }

    constexpr std::size_t kFixtureBytes = 64U * 1024U;
    const std::vector<std::byte> fixture = make_fixture(kFixtureBytes);
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(
        zevryon::core::ResourceClass::UnicodeBuffer,
        unicode_budget_bytes);
    zevryon::core::LedgerMemoryResource memory(
        ledger, zevryon::core::ResourceClass::UnicodeBuffer);
    std::pmr::vector<zevryon::text::DecodedCodePoint> output(&memory);
    std::vector<double> samples_ms;
    samples_ms.reserve(iterations);
    std::size_t expected_codepoints = 0U;

    for (std::size_t iteration = 0U; iteration < iterations + 32U; ++iteration) {
        output.clear();
        zevryon::text::Utf8StreamDecoder decoder(
            zevryon::text::Utf8ErrorPolicy::Strict);
        zevryon::text::Utf8DecodeError error;
        const auto started = std::chrono::steady_clock::now();
        std::size_t consumed = 0U;
        while (consumed < fixture.size()) {
            const std::size_t count =
                std::min(chunk_bytes, fixture.size() - consumed);
            if (!decoder.feed(
                    std::span<const std::byte>(fixture.data() + consumed, count),
                    static_cast<std::uint64_t>(consumed),
                    &output,
                    &error)) {
                std::cerr << "decode failed: "
                          << zevryon::text::utf8_error_kind_name(error.kind)
                          << " at " << error.source_offset << ' '
                          << error.message << '\n';
                return 1;
            }
            consumed += count;
        }
        if (!decoder.finish(&output, &error)) {
            std::cerr << "finish failed: "
                      << zevryon::text::utf8_error_kind_name(error.kind)
                      << " at " << error.source_offset << ' '
                      << error.message << '\n';
            return 1;
        }
        const auto ended = std::chrono::steady_clock::now();
        if (decoder.stats().source_bytes != kFixtureBytes ||
            decoder.stats().invalid_sequences != 0U ||
            decoder.stats().replacements != 0U || output.empty()) {
            std::cerr << "decoder statistics failed benchmark contract\n";
            return 1;
        }
        if (expected_codepoints == 0U) {
            expected_codepoints = output.size();
        } else if (output.size() != expected_codepoints) {
            std::cerr << "decoded codepoint count changed between iterations\n";
            return 1;
        }
        if (iteration >= 32U) {
            samples_ms.push_back(
                std::chrono::duration<double, std::milli>(ended - started).count());
        }
    }

    const auto resource =
        ledger.snapshot(zevryon::core::ResourceClass::UnicodeBuffer);
    const double p50 = percentile(samples_ms, 50.0);
    const double p95 = percentile(samples_ms, 95.0);
    const double p99 = percentile(samples_ms, 99.0);
    const double maximum = *std::max_element(samples_ms.begin(), samples_ms.end());
    const double throughput_mib_per_second =
        (static_cast<double>(kFixtureBytes) / (1024.0 * 1024.0)) /
        (p50 / 1000.0);

    std::cout << '{'
              << "\"schema\":\"zevryon.unicode-benchmark.v1\","
              << "\"fixture_bytes\":" << kFixtureBytes << ','
              << "\"iterations\":" << iterations << ','
              << "\"warmup_iterations\":32,"
              << "\"chunk_bytes\":" << chunk_bytes << ','
              << "\"decoded_codepoints\":" << expected_codepoints << ','
              << "\"p50_ms\":" << p50 << ','
              << "\"p95_ms\":" << p95 << ','
              << "\"p99_ms\":" << p99 << ','
              << "\"maximum_ms\":" << maximum << ','
              << "\"p50_mib_per_second\":" << throughput_mib_per_second << ','
              << "\"unicode_hard_limit_bytes\":" << resource.hard_limit_bytes << ','
              << "\"unicode_current_bytes\":" << resource.current_bytes << ','
              << "\"unicode_peak_bytes\":" << resource.peak_bytes << ','
              << "\"rejected_reservations\":" << resource.rejected_reservations << ','
              << "\"accounting_errors\":" << resource.accounting_errors << ','
              << "\"within_hard_limits\":"
              << (ledger.within_hard_limits() ? "true" : "false") << ','
              << "\"accounting_clean\":"
              << (ledger.accounting_clean() ? "true" : "false")
              << "}\n";
    return 0;
}
