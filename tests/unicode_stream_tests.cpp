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
#if defined(ZEVRYON_UTF8_RUST_SHADOW)
    if (!require(decoder.rust_shadow_enabled(), "decode Rust shadow initializes") ||
        !require(decoder.rust_shadow_healthy(), "decode Rust shadow remains healthy") ||
        !require(
            decoder.rust_shadow_mismatches() == 0U,
            "decode Rust shadow records zero mismatches")) {
        return false;
    }
#endif
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
    const bool rejected = !decoder.feed(input, source_base, &output, &error);
#if defined(ZEVRYON_UTF8_RUST_SHADOW)
    if (!require(decoder.rust_shadow_healthy(), "strict Rust shadow remains healthy") ||
        !require(
            decoder.rust_shadow_mismatches() == 0U,
            "strict Rust shadow records zero mismatches")) {
        return false;
    }
#endif
    return require(rejected, "strict input rejected") &&
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
    const bool result =
        decoder.feed(first, 0U, &output, &error) &&
        decoder.finish(&output, &error) &&
        decoder.finish(&output, &error) &&
        !decoder.feed(second, 1U, &output, &error);
#if defined(ZEVRYON_UTF8_RUST_SHADOW)
    if (!require(decoder.rust_shadow_healthy(), "lifecycle Rust shadow remains healthy") ||
        !require(
            decoder.rust_shadow_operations() >= 6U,
            "lifecycle Rust shadow observes feed finish and reset operations") ||
        !require(
            decoder.rust_shadow_verifications() >= decoder.rust_shadow_operations(),
            "lifecycle Rust shadow verifies every operation") ||
        !require(
            decoder.rust_shadow_mismatches() == 0U,
            "lifecycle Rust shadow records zero mismatches")) {
        return false;
    }
#endif
    return require(result, "reset finish and post-finish lifecycle contract");
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
#if defined(ZEVRYON_UTF8_RUST_SHADOW)
        if (!require(
                decoder.rust_shadow_healthy(),
                "budget failure is mirrored without false divergence") ||
            !require(
                decoder.rust_shadow_mismatches() == 0U,
                "budget failure records zero Rust mismatches")) {
            return false;
        }
#endif
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
#if defined(ZEVRYON_UTF8_RUST_SHADOW)
        if (!require(decoder.rust_shadow_healthy(), "budgeted Rust shadow remains healthy")) {
            return false;
        }
#endif
    }
    return require(
               released.snapshot(zevryon::core::ResourceClass::UnicodeBuffer)
                       .current_bytes == 0U,
               "PMR destruction releases allocation") &&
           require(released.accounting_clean(), "PMR accounting remains clean");
}

bool test_shadow_telemetry_contract() {
    Utf8StreamDecoder decoder(Utf8ErrorPolicy::Replace);
#if defined(ZEVRYON_UTF8_RUST_SHADOW)
    if (!require(decoder.rust_shadow_enabled(), "Rust Unicode verifier is enabled") ||
        !require(decoder.rust_shadow_healthy(), "fresh Unicode verifier is healthy") ||
        !require(decoder.rust_shadow_operations() == 0U, "fresh verifier has zero operations") ||
        !require(decoder.rust_shadow_verifications() >= 1U, "constructor verifies Rust state") ||
        !require(decoder.rust_shadow_mismatches() == 0U, "fresh verifier has zero mismatches")) {
        return false;
    }

    const std::string telemetry = decoder.rust_shadow_json();
#if defined(ZEVRYON_UTF8_RUST_AUTHORITATIVE)
    return require(
               telemetry.find("\"schema\":\"zevryon.rust-unicode-authority.v1\"") !=
                   std::string::npos,
               "authority telemetry exposes schema") &&
           require(
               telemetry.find("\"authoritative_backend\":\"rust\"") !=
                   std::string::npos,
               "authority telemetry reports Rust backend") &&
           require(
               telemetry.find("\"reverse_shadow_backend\":\"cpp\"") !=
                   std::string::npos,
               "authority telemetry reports C++ reverse shadow") &&
           require(
               telemetry.find("\"fallback_permitted\":false") !=
                   std::string::npos,
               "authority telemetry forbids fallback") &&
           require(
               telemetry.find("\"enabled\":true") != std::string::npos,
               "authority telemetry reports enabled");
#else
    return require(
               telemetry.find("zevryon.rust-unicode-shadow.v1") !=
                   std::string::npos,
               "shadow telemetry exposes schema") &&
           require(
               telemetry.find("\"enabled\":true") != std::string::npos,
               "shadow telemetry reports enabled");
#endif
#else
    return require(!decoder.rust_shadow_enabled(), "default C++ build keeps Unicode Rust off") &&
           require(!decoder.rust_shadow_healthy(), "disabled Unicode shadow is not reported healthy") &&
           require(decoder.rust_shadow_operations() == 0U, "disabled shadow has zero operations") &&
           require(decoder.rust_shadow_verifications() == 0U, "disabled shadow has zero verifications") &&
           require(decoder.rust_shadow_mismatches() == 0U, "disabled shadow has zero mismatches") &&
           require(
               decoder.rust_shadow_json().find("\"enabled\":false") !=
                   std::string::npos,
               "default telemetry reports disabled");
#endif
}

} // namespace

int main() {
    if (!test_chunk_equivalence() ||
        !test_strict_errors() ||
        !test_replacement_policy() ||
        !test_lifecycle() ||
        !test_resource_budget() ||
        !test_shadow_telemetry_contract()) {
        return 1;
    }
    std::cout << "Unicode stream tests passed\n";
    return 0;
}
