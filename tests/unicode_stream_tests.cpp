#include "ledger_memory_resource.hpp"
#include "unicode_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace {

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

std::vector<std::byte> bytes(std::initializer_list<unsigned int> values) {
    std::vector<std::byte> output;
    output.reserve(values.size());
    for (const unsigned int value : values) {
        output.push_back(static_cast<std::byte>(value));
    }
    return output;
}

bool decode_with_chunks(
    const std::vector<std::byte>& input,
    std::size_t chunk_bytes,
    std::uint64_t source_base,
    zevryon::text::Utf8ErrorPolicy policy,
    std::vector<zevryon::text::DecodedCodePoint>* result,
    zevryon::text::Utf8DecodeError* error) {
    if (result == nullptr || error == nullptr || chunk_bytes == 0U) {
        return false;
    }
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(zevryon::core::ResourceClass::UnicodeBuffer, 1U << 20U);
    zevryon::core::LedgerMemoryResource memory(
        ledger, zevryon::core::ResourceClass::UnicodeBuffer);
    std::pmr::vector<zevryon::text::DecodedCodePoint> output(&memory);
    zevryon::text::Utf8StreamDecoder decoder(policy);

    std::size_t consumed = 0U;
    while (consumed < input.size()) {
        const std::size_t count = std::min(chunk_bytes, input.size() - consumed);
        if (!decoder.feed(
                std::span<const std::byte>(input.data() + consumed, count),
                source_base + static_cast<std::uint64_t>(consumed),
                &output,
                error)) {
            return false;
        }
        consumed += count;
    }
    if (!decoder.finish(&output, error)) {
        return false;
    }
    result->assign(output.begin(), output.end());
    return true;
}

bool test_chunk_equivalence() {
    const std::vector<std::byte> input = bytes({
        0x41U,
        0xc5U, 0x9fU,
        0x65U, 0xccU, 0x81U,
        0xf0U, 0x9fU, 0x98U, 0x80U,
        0x0aU,
        0xe4U, 0xb8U, 0xadU,
    });
    std::vector<zevryon::text::DecodedCodePoint> reference;
    zevryon::text::Utf8DecodeError error;
    if (!require(
            decode_with_chunks(
                input,
                input.size(),
                1000U,
                zevryon::text::Utf8ErrorPolicy::Strict,
                &reference,
                &error),
            "one-shot UTF-8 decode succeeds")) {
        return false;
    }

    for (std::size_t chunk = 1U; chunk <= input.size(); ++chunk) {
        std::vector<zevryon::text::DecodedCodePoint> candidate;
        if (!require(
                decode_with_chunks(
                    input,
                    chunk,
                    1000U,
                    zevryon::text::Utf8ErrorPolicy::Strict,
                    &candidate,
                    &error),
                "chunked UTF-8 decode succeeds") ||
            !require(candidate == reference, "chunk boundaries do not change decoded output")) {
            return false;
        }
    }

    if (!require(reference.size() == 8U, "expected codepoint count") ||
        !require(reference[0].value == 0x41U, "ASCII codepoint") ||
        !require(reference[1].value == 0x15fU, "Turkish codepoint") ||
        !require(reference[3].value == 0x301U, "combining mark codepoint") ||
        !require(reference[4].value == 0x1f600U, "four-byte emoji codepoint") ||
        !require(reference[1].source_start == 1001U, "multibyte source start preserved") ||
        !require(reference[1].source_end == 1003U, "multibyte source end preserved") ||
        !require(reference[4].source_end - reference[4].source_start == 4U, "emoji byte range preserved")) {
        return false;
    }
    return true;
}

bool test_strict_errors() {
    const std::vector<std::byte> invalid_continuation = bytes({0xe2U, 0x28U, 0xa1U});
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(zevryon::core::ResourceClass::UnicodeBuffer, 4096U);
    zevryon::core::LedgerMemoryResource memory(
        ledger, zevryon::core::ResourceClass::UnicodeBuffer);
    std::pmr::vector<zevryon::text::DecodedCodePoint> output(&memory);
    zevryon::text::Utf8StreamDecoder decoder(zevryon::text::Utf8ErrorPolicy::Strict);
    zevryon::text::Utf8DecodeError error;
    if (!require(
            !decoder.feed(invalid_continuation, 0U, &output, &error),
            "strict decoder rejects invalid continuation") ||
        !require(
            error.kind == zevryon::text::Utf8ErrorKind::InvalidContinuation,
            "invalid continuation error kind") ||
        !require(error.source_offset == 1U, "invalid continuation offset") ||
        !require(decoder.failed(), "strict decoder enters failed state")) {
        return false;
    }

    const std::vector<std::byte> surrogate = bytes({0xedU, 0xa0U, 0x80U});
    output.clear();
    decoder.reset();
    if (!require(!decoder.feed(surrogate, 50U, &output, &error), "strict decoder rejects surrogate") ||
        !require(
            error.kind == zevryon::text::Utf8ErrorKind::SurrogateCodePoint,
            "surrogate error kind") ||
        !require(error.source_offset == 50U, "surrogate sequence start reported")) {
        return false;
    }

    const std::vector<std::byte> out_of_range = bytes({0xf4U, 0x90U, 0x80U, 0x80U});
    output.clear();
    decoder.reset();
    if (!require(
            !decoder.feed(out_of_range, 100U, &output, &error),
            "strict decoder rejects out-of-range codepoint") ||
        !require(
            error.kind == zevryon::text::Utf8ErrorKind::CodePointOutOfRange,
            "out-of-range error kind")) {
        return false;
    }

    const std::vector<std::byte> overlong = bytes({0xe0U, 0x80U, 0x80U});
    output.clear();
    decoder.reset();
    if (!require(!decoder.feed(overlong, 0U, &output, &error), "strict decoder rejects overlong encoding") ||
        !require(
            error.kind == zevryon::text::Utf8ErrorKind::OverlongEncoding,
            "overlong error kind")) {
        return false;
    }
    return true;
}

bool test_replacement_policy() {
    const std::vector<std::byte> input = bytes({0xe2U, 0x28U, 0xa1U, 0x41U});
    std::vector<zevryon::text::DecodedCodePoint> decoded;
    zevryon::text::Utf8DecodeError error;
    if (!require(
            decode_with_chunks(
                input,
                1U,
                200U,
                zevryon::text::Utf8ErrorPolicy::Replace,
                &decoded,
                &error),
            "replacement policy accepts malformed input") ||
        !require(decoded.size() == 4U, "replacement output count") ||
        !require(decoded[0].replacement && decoded[0].value == 0xfffdU, "broken sequence replaced") ||
        !require(decoded[0].source_start == 200U && decoded[0].source_end == 201U, "broken lead range") ||
        !require(decoded[1].value == 0x28U, "invalid continuation retried as ASCII") ||
        !require(decoded[2].replacement, "unexpected continuation replaced") ||
        !require(decoded[2].source_start == 202U && decoded[2].source_end == 203U, "continuation range") ||
        !require(decoded[3].value == 0x41U, "decode continues after replacement")) {
        return false;
    }

    const std::vector<std::byte> truncated = bytes({0xf0U, 0x9fU});
    decoded.clear();
    if (!require(
            decode_with_chunks(
                truncated,
                1U,
                300U,
                zevryon::text::Utf8ErrorPolicy::Replace,
                &decoded,
                &error),
            "replacement policy closes truncated sequence") ||
        !require(decoded.size() == 1U && decoded[0].replacement, "truncated sequence replaced once") ||
        !require(decoded[0].source_start == 300U && decoded[0].source_end == 302U, "truncated range preserved")) {
        return false;
    }
    return true;
}

bool test_contiguity_and_finish() {
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(zevryon::core::ResourceClass::UnicodeBuffer, 4096U);
    zevryon::core::LedgerMemoryResource memory(
        ledger, zevryon::core::ResourceClass::UnicodeBuffer);
    std::pmr::vector<zevryon::text::DecodedCodePoint> output(&memory);
    zevryon::text::Utf8StreamDecoder decoder;
    zevryon::text::Utf8DecodeError error;
    const std::vector<std::byte> first = bytes({0x41U});
    const std::vector<std::byte> second = bytes({0x42U});
    if (!require(decoder.feed(first, 10U, &output, &error), "first contiguous chunk accepted") ||
        !require(
            !decoder.feed(second, 12U, &output, &error),
            "discontinuous chunk rejected") ||
        !require(
            error.kind == zevryon::text::Utf8ErrorKind::DiscontinuousInput,
            "discontinuous error kind")) {
        return false;
    }

    decoder.reset();
    output.clear();
    if (!require(decoder.feed(first, 0U, &output, &error), "decoder accepts input after reset") ||
        !require(decoder.finish(&output, &error), "finish succeeds") ||
        !require(decoder.finish(&output, &error), "finish is idempotent") ||
        !require(
            !decoder.feed(second, 1U, &output, &error),
            "input after finish is rejected")) {
        return false;
    }
    return true;
}

bool test_resource_budget() {
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(zevryon::core::ResourceClass::UnicodeBuffer, 1U);
    zevryon::text::Utf8DecodeError error;
    {
        zevryon::core::LedgerMemoryResource memory(
            ledger, zevryon::core::ResourceClass::UnicodeBuffer);
        std::pmr::vector<zevryon::text::DecodedCodePoint> output(&memory);
        zevryon::text::Utf8StreamDecoder decoder;
        const std::vector<std::byte> input = bytes({0x41U});
        if (!require(
                !decoder.feed(input, 0U, &output, &error),
                "Unicode output hard cap rejects allocation") ||
            !require(
                error.kind == zevryon::text::Utf8ErrorKind::OutputBudgetExceeded,
                "budget error kind") ||
            !require(
                ledger.snapshot(zevryon::core::ResourceClass::UnicodeBuffer)
                        .rejected_reservations >= 1U,
                "ledger records rejected Unicode allocation") ||
            !require(
                ledger.snapshot(zevryon::core::ResourceClass::UnicodeBuffer)
                        .current_bytes == 0U,
                "rejected allocation does not consume budget")) {
            return false;
        }
    }

    zevryon::core::ResourceLedger release_ledger;
    release_ledger.set_hard_limit(
        zevryon::core::ResourceClass::UnicodeBuffer, 4096U);
    {
        zevryon::core::LedgerMemoryResource memory(
            release_ledger, zevryon::core::ResourceClass::UnicodeBuffer);
        std::pmr::vector<zevryon::text::DecodedCodePoint> output(&memory);
        zevryon::text::Utf8StreamDecoder decoder;
        const std::vector<std::byte> input = bytes({0x41U, 0x42U, 0x43U});
        if (!require(decoder.feed(input, 0U, &output, &error), "budgeted output succeeds") ||
            !require(decoder.finish(&output, &error), "budgeted output finishes") ||
            !require(
                release_ledger.snapshot(zevryon::core::ResourceClass::UnicodeBuffer)
                        .current_bytes > 0U,
                "actual PMR allocation is charged")) {
            return false;
        }
    }
    if (!require(
            release_ledger.snapshot(zevryon::core::ResourceClass::UnicodeBuffer)
                    .current_bytes == 0U,
            "PMR destruction releases exact allocation") ||
        !require(release_ledger.accounting_clean(), "PMR accounting remains clean")) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!test_chunk_equivalence() ||
        !test_strict_errors() ||
        !test_replacement_policy() ||
        !test_contiguity_and_finish() ||
        !test_resource_budget()) {
        return 1;
    }
    std::cout << "Unicode stream tests passed\n";
    return 0;
}
