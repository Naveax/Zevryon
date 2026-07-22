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

using zevryon::text::DecodedCodePoint;
using zevryon::text::Utf8DecodeError;
using zevryon::text::Utf8ErrorKind;
using zevryon::text::Utf8ErrorPolicy;
using zevryon::text::Utf8StreamDecoder;

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

bool decode(
    const std::vector<std::byte>& input,
    std::size_t chunk_bytes,
    std::uint64_t source_base,
    Utf8ErrorPolicy policy,
    std::vector<DecodedCodePoint>* result,
    Utf8DecodeError* error) {
    if (result == nullptr || error == nullptr || chunk_bytes == 0U) {
        return false;
    }
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(zevryon::core::ResourceClass::UnicodeBuffer, 1U << 20U);
    zevryon::core::LedgerMemoryResource memory(
        ledger, zevryon::core::ResourceClass::UnicodeBuffer);
    std::pmr::vector<DecodedCodePoint> output(&memory);
    Utf8StreamDecoder decoder(policy);

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

bool ranges_are_valid(const std::vector<DecodedCodePoint>& codepoints) {
    for (const DecodedCodePoint& codepoint : codepoints) {
        if (codepoint.source_length == 0U || codepoint.source_length > 4U ||
            codepoint.source_end() <= codepoint.source_start) {
            return false;
        }
    }
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
    std::vector<DecodedCodePoint> reference;
    Utf8DecodeError error;
    if (!require(
            decode(
                input,
                input.size(),
                1000U,
                Utf8ErrorPolicy::Strict,
                &reference,
                &error),
            "one-shot UTF-8 decode succeeds")) {
        return false;
    }

    for (std::size_t chunk = 1U; chunk <= input.size(); ++chunk) {
        std::vector<DecodedCodePoint> candidate;
        if (!require(
                decode(
                    input,
                    chunk,
                    1000U,
                    Utf8ErrorPolicy::Strict,
                    &candidate,
                    &error),
                "chunked UTF-8 decode succeeds") ||
            !require(candidate == reference, "chunk boundaries do not change output")) {
            return false;
        }
    }

    return require(sizeof(DecodedCodePoint) <= 16U, "codepoint record stays within 16 bytes") &&
           require(reference.size() == 7U, "expected codepoint count") &&
           require(ranges_are_valid(reference), "all source lengths remain within UTF-8 bounds") &&
           require(reference[0].value == 0x41U, "ASCII codepoint") &&
           require(reference[1].value == 0x15fU, "Turkish codepoint") &&
           require(reference[3].value == 0x301U, "combining mark") &&
           require(reference[4].value == 0x1f600U, "emoji codepoint") &&
           require(reference[1].source_start == 1001U, "source start preserved") &&
           require(reference[1].source_end() == 1003U, "source end derived correctly") &&
           require(reference[4].source_length == 4U, "emoji byte length preserved");
}

bool expect_strict_error(
    const std::vector<std::byte>& input,
    std::uint64_t source_base,
    Utf8ErrorKind expected_kind,
    std::uint64_t expected_offset) {
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(zevryon::core::ResourceClass::UnicodeBuffer, 4096U);
    zevryon::core::LedgerMemoryResource memory(
        ledger, zevryon::core::ResourceClass::UnicodeBuffer);
    std::pmr::vector<DecodedCodePoint> output(&memory);
    Utf8StreamDecoder decoder(Utf8ErrorPolicy::Strict);
    Utf8DecodeError error;
    return require(!decoder.feed(input, source_base, &output, &error), "strict input rejected") &&
           require(error.kind == expected_kind, "strict error kind") &&
           require(error.source_offset == expected_offset, "strict error offset") &&
           require(decoder.failed(), "strict decoder enters failed state");
}

bool test_strict_errors() {
    return expect_strict_error(
               bytes({0xe2U, 0x28U, 0xa1U}),
               0U,
               Utf8ErrorKind::InvalidContinuation,
               1U) &&
           expect_strict_error(
               bytes({0xedU, 0xa0U, 0x80U}),
               50U,
               Utf8ErrorKind::SurrogateCodePoint,
               50U) &&
           expect_strict_error(
               bytes({0xf4U, 0x90U, 0x80U, 0x80U}),
               100U,
               Utf8ErrorKind::CodePointOutOfRange,
               100U) &&
           expect_strict_error(
               bytes({0xe0U, 0x80U, 0x80U}),
               0U,
               Utf8ErrorKind::OverlongEncoding,
               0U) &&
           expect_strict_error(
               bytes({0x80U}),
               500U,
               Utf8ErrorKind::UnexpectedContinuation,
               500U) &&
           expect_strict_error(
               bytes({0xffU}),
               700U,
               Utf8ErrorKind::InvalidLeadByte,
               700U);
}

bool test_replacement_policy() {
    Utf8DecodeError error;
    std::vector<DecodedCodePoint> decoded;
    const std::vector<std::byte> input = bytes({0xe2U, 0x28U, 0xa1U, 0x41U});
    if (!require(
            decode(
                input,
                1U,
                200U,
                Utf8ErrorPolicy::Replace,
                &decoded,
                &error),
            "replacement policy accepts malformed input")) {
        return false;
    }
    if (!require(decoded.size() == 4U, "replacement output count") ||
        !require(decoded[0].replacement && decoded[0].value == 0xfffdU, "broken sequence replaced") ||
        !require(decoded[0].source_start == 200U && decoded[0].source_end() == 201U, "broken lead range") ||
        !require(decoded[1].value == 0x28U, "non-continuation retried as ASCII") ||
        !require(decoded[2].replacement, "unexpected continuation replaced") ||
        !require(decoded[2].source_start == 202U && decoded[2].source_end() == 203U, "continuation range") ||
        !require(decoded[3].value == 0x41U, "decode continues after replacement") ||
        !require(ranges_are_valid(decoded), "replacement ranges remain compact and valid")) {
        return false;
    }

    decoded.clear();
    const std::vector<std::byte> truncated = bytes({0xf0U, 0x9fU});
    return require(
               decode(
                   truncated,
                   1U,
                   300U,
                   Utf8ErrorPolicy::Replace,
                   &decoded,
                   &error),
               "replacement policy closes truncated sequence") &&
           require(decoded.size() == 1U && decoded[0].replacement, "truncated sequence replaced once") &&
           require(decoded[0].source_start == 300U && decoded[0].source_end() == 302U, "truncated range preserved");
}

bool test_lifecycle() {
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(zevryon::core::ResourceClass::UnicodeBuffer, 4096U);
    zevryon::core::LedgerMemoryResource memory(
        ledger, zevryon::core::ResourceClass::UnicodeBuffer);
    std::pmr::vector<DecodedCodePoint> output(&memory);
    Utf8StreamDecoder decoder;
    Utf8DecodeError error;
    const std::vector<std::byte> first = bytes({0x41U});
    const std::vector<std::byte> second = bytes({0x42U});
    if (!require(decoder.feed(first, 10U, &output, &error), "first chunk accepted") ||
        !require(!decoder.feed(second, 12U, &output, &error), "discontinuous chunk rejected") ||
        !require(error.kind == Utf8ErrorKind::DiscontinuousInput, "discontinuous error kind")) {
        return false;
    }

    decoder.reset();
    output.clear();
    return require(decoder.feed(first, 0U, &output, &error), "input accepted after reset") &&
           require(decoder.finish(&output, &error), "finish succeeds") &&
           require(decoder.finish(&output, &error), "finish is idempotent") &&
           require(!decoder.feed(second, 1U, &output, &error), "input after finish rejected");
}

bool test_resource_budget() {
    Utf8DecodeError error;
    zevryon::core::ResourceLedger rejected;
    rejected.set_hard_limit(zevryon::core::ResourceClass::UnicodeBuffer, 1U);
    {
        zevryon::core::LedgerMemoryResource memory(
            rejected, zevryon::core::ResourceClass::UnicodeBuffer);
        std::pmr::vector<DecodedCodePoint> output(&memory);
        Utf8StreamDecoder decoder;
        const std::vector<std::byte> input = bytes({0x41U});
        if (!require(!decoder.feed(input, 0U, &output, &error), "hard cap rejects output") ||
            !require(error.kind == Utf8ErrorKind::OutputBudgetExceeded, "budget error kind") ||
            !require(
                rejected.snapshot(zevryon::core::ResourceClass::UnicodeBuffer)
                        .rejected_reservations >= 1U,
                "rejected allocation recorded") ||
            !require(
                rejected.snapshot(zevryon::core::ResourceClass::UnicodeBuffer)
                        .current_bytes == 0U,
                "rejected allocation consumes no budget")) {
            return false;
        }
    }

    zevryon::core::ResourceLedger released;
    released.set_hard_limit(zevryon::core::ResourceClass::UnicodeBuffer, 4096U);
    {
        zevryon::core::LedgerMemoryResource memory(
            released, zevryon::core::ResourceClass::UnicodeBuffer);
        std::pmr::vector<DecodedCodePoint> output(&memory);
        Utf8StreamDecoder decoder;
        const std::vector<std::byte> input = bytes({0x41U, 0x42U, 0x43U});
        if (!require(decoder.feed(input, 0U, &output, &error), "budgeted output succeeds") ||
            !require(decoder.finish(&output, &error), "budgeted output finishes") ||
            !require(
                released.snapshot(zevryon::core::ResourceClass::UnicodeBuffer)
                        .current_bytes > 0U,
                "actual allocation charged")) {
            return false;
        }
    }
    return require(
               released.snapshot(zevryon::core::ResourceClass::UnicodeBuffer)
                       .current_bytes == 0U,
               "PMR destruction releases allocation") &&
           require(released.accounting_clean(), "PMR accounting remains clean");
}

} // namespace

int main() {
    if (!test_chunk_equivalence() ||
        !test_strict_errors() ||
        !test_replacement_policy() ||
        !test_lifecycle() ||
        !test_resource_budget()) {
        return 1;
    }
    std::cout << "Unicode stream tests passed\n";
    return 0;
}
