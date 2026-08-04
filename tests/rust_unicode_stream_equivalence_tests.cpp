#include "rust_unicode_stream_decoder.hpp"
#include "unicode_stream.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace {

using zevryon::text::DecodedCodePoint;
using zevryon::text::RustUtf8StreamDecoder;
using zevryon::text::Utf8DecodeError;
using zevryon::text::Utf8DecodeStats;
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

DecodedCodePoint convert(const ZrDecodedCodePoint& point) {
    return {
        point.value,
        point.source_start,
        point.source_start + static_cast<std::uint64_t>(point.source_length),
        point.replacement != 0U,
    };
}

bool same_stats(const Utf8DecodeStats& left, const Utf8DecodeStats& right) {
    return left.source_bytes == right.source_bytes &&
           left.emitted_codepoints == right.emitted_codepoints &&
           left.invalid_sequences == right.invalid_sequences &&
           left.replacements == right.replacements &&
           left.chunks == right.chunks &&
           left.maximum_pending_continuations ==
               right.maximum_pending_continuations;
}

bool compare_state(
    const Utf8StreamDecoder& cpp,
    const RustUtf8StreamDecoder& rust,
    const std::pmr::vector<DecodedCodePoint>& cpp_output,
    const std::vector<DecodedCodePoint>& rust_output,
    const Utf8DecodeError& cpp_error,
    const Utf8DecodeError& rust_error,
    bool cpp_result,
    bool rust_result,
    const std::string& context) {
    return require(cpp_result == rust_result, context + ": return result") &&
           require(
               std::equal(
                   cpp_output.begin(),
                   cpp_output.end(),
                   rust_output.begin(),
                   rust_output.end()),
               context + ": decoded records") &&
           require(cpp_output.size() == rust_output.size(), context + ": output size") &&
           require(cpp_error.kind == rust_error.kind, context + ": error kind") &&
           require(
               cpp_error.source_offset == rust_error.source_offset,
               context + ": error offset") &&
           require(same_stats(cpp.stats(), rust.stats()), context + ": stats") &&
           require(
               cpp.next_source_offset() == rust.next_source_offset(),
               context + ": next source offset") &&
           require(cpp.failed() == rust.failed(), context + ": failed state") &&
           require(cpp.policy() == rust.policy(), context + ": policy");
}

bool run_case(
    const std::vector<std::byte>& input,
    std::size_t chunk_bytes,
    std::uint64_t source_base,
    Utf8ErrorPolicy policy,
    bool finish_after_feed,
    const std::string& name) {
    std::pmr::vector<DecodedCodePoint> cpp_output;
    std::vector<DecodedCodePoint> rust_output;
    Utf8StreamDecoder cpp(policy);
    RustUtf8StreamDecoder rust(policy);
    Utf8DecodeError cpp_error;
    Utf8DecodeError rust_error;

    if (!require(rust.valid(), name + ": Rust decoder initializes") ||
        !require(
            rust.policy() == policy,
            name + ": Rust policy initializes exactly")) {
        return false;
    }

    std::size_t consumed = 0U;
    while (consumed < input.size()) {
        const std::size_t count = std::min(chunk_bytes, input.size() - consumed);
        const std::span<const std::byte> chunk(input.data() + consumed, count);
        const std::uint64_t offset =
            source_base + static_cast<std::uint64_t>(consumed);
        const bool cpp_result = cpp.feed(chunk, offset, &cpp_output, &cpp_error);

        std::vector<ZrDecodedCodePoint> ffi_output(count * 2U + 2U);
        std::size_t written = 0U;
        const bool rust_result = rust.feed(
            chunk,
            offset,
            ffi_output,
            &written,
            &rust_error);
        if (written > ffi_output.size()) {
            return require(false, name + ": Rust write count is bounded");
        }
        for (std::size_t index = 0U; index < written; ++index) {
            rust_output.push_back(convert(ffi_output[index]));
        }

        if (!compare_state(
                cpp,
                rust,
                cpp_output,
                rust_output,
                cpp_error,
                rust_error,
                cpp_result,
                rust_result,
                name + ": feed")) {
            return false;
        }
        if (!cpp_result) {
            return true;
        }
        consumed += count;
    }

    if (!finish_after_feed) {
        return true;
    }

    const bool cpp_result = cpp.finish(&cpp_output, &cpp_error);
    std::array<ZrDecodedCodePoint, 2> ffi_output{};
    std::size_t written = 0U;
    const bool rust_result = rust.finish(
        ffi_output,
        &written,
        &rust_error);
    for (std::size_t index = 0U; index < written; ++index) {
        rust_output.push_back(convert(ffi_output[index]));
    }
    return compare_state(
        cpp,
        rust,
        cpp_output,
        rust_output,
        cpp_error,
        rust_error,
        cpp_result,
        rust_result,
        name + ": finish");
}

bool test_valid_chunk_equivalence() {
    const std::vector<std::byte> input = bytes({
        0x41U,
        0xc5U, 0x9fU,
        0x65U, 0xccU, 0x81U,
        0xf0U, 0x9fU, 0x98U, 0x80U,
        0x0aU,
        0xe4U, 0xb8U, 0xadU,
    });
    for (std::size_t chunk = 1U; chunk <= input.size(); ++chunk) {
        if (!run_case(
                input,
                chunk,
                1000U,
                Utf8ErrorPolicy::Strict,
                true,
                "valid chunk " + std::to_string(chunk))) {
            return false;
        }
    }
    return true;
}

bool test_strict_error_equivalence() {
    struct Case {
        std::vector<std::byte> input;
        std::uint64_t base;
    };
    const std::array<Case, 6> cases{{
        {bytes({0xe2U, 0x28U, 0xa1U}), 0U},
        {bytes({0xedU, 0xa0U, 0x80U}), 50U},
        {bytes({0xf4U, 0x90U, 0x80U, 0x80U}), 100U},
        {bytes({0xe0U, 0x80U, 0x80U}), 0U},
        {bytes({0x80U}), 500U},
        {bytes({0xffU}), 700U},
    }};

    for (std::size_t index = 0U; index < cases.size(); ++index) {
        for (std::size_t chunk = 1U; chunk <= cases[index].input.size(); ++chunk) {
            if (!run_case(
                    cases[index].input,
                    chunk,
                    cases[index].base,
                    Utf8ErrorPolicy::Strict,
                    true,
                    "strict case " + std::to_string(index))) {
                return false;
            }
        }
    }
    return true;
}

bool test_replacement_equivalence() {
    const std::vector<std::vector<std::byte>> cases{
        bytes({0xe2U, 0x28U, 0xa1U, 0x41U}),
        bytes({0xf0U, 0x9fU}),
        bytes({0xedU, 0xa0U, 0x80U, 0x42U}),
        bytes({0xf4U, 0x90U, 0x80U, 0x80U, 0x43U}),
        bytes({0xe0U, 0x80U, 0x80U, 0x44U}),
        bytes({0xffU, 0x80U, 0x45U}),
    };
    for (std::size_t index = 0U; index < cases.size(); ++index) {
        for (std::size_t chunk = 1U; chunk <= cases[index].size(); ++chunk) {
            if (!run_case(
                    cases[index],
                    chunk,
                    200U + static_cast<std::uint64_t>(index) * 100U,
                    Utf8ErrorPolicy::Replace,
                    true,
                    "replacement case " + std::to_string(index))) {
                return false;
            }
        }
    }
    return true;
}

bool test_lifecycle_equivalence() {
    std::pmr::vector<DecodedCodePoint> cpp_output;
    std::vector<DecodedCodePoint> rust_output;
    Utf8StreamDecoder cpp(Utf8ErrorPolicy::Strict);
    RustUtf8StreamDecoder rust(Utf8ErrorPolicy::Strict);
    Utf8DecodeError cpp_error;
    Utf8DecodeError rust_error;
    const auto first = bytes({0x41U});
    const auto second = bytes({0x42U});

    bool cpp_result = cpp.feed(first, 10U, &cpp_output, &cpp_error);
    std::array<ZrDecodedCodePoint, 4> ffi_output{};
    std::size_t written = 0U;
    bool rust_result = rust.feed(first, 10U, ffi_output, &written, &rust_error);
    for (std::size_t index = 0U; index < written; ++index) {
        rust_output.push_back(convert(ffi_output[index]));
    }
    if (!compare_state(
            cpp,
            rust,
            cpp_output,
            rust_output,
            cpp_error,
            rust_error,
            cpp_result,
            rust_result,
            "lifecycle first feed")) {
        return false;
    }

    cpp_result = cpp.feed(second, 12U, &cpp_output, &cpp_error);
    rust_result = rust.feed(second, 12U, ffi_output, &written, &rust_error);
    if (!compare_state(
            cpp,
            rust,
            cpp_output,
            rust_output,
            cpp_error,
            rust_error,
            cpp_result,
            rust_result,
            "lifecycle discontinuity")) {
        return false;
    }

    cpp.reset();
    if (!require(rust.reset(), "Rust reset succeeds")) {
        return false;
    }
    cpp_output.clear();
    rust_output.clear();
    cpp_error = {};
    rust_error = {};

    cpp_result = cpp.feed(first, 0U, &cpp_output, &cpp_error);
    rust_result = rust.feed(first, 0U, ffi_output, &written, &rust_error);
    for (std::size_t index = 0U; index < written; ++index) {
        rust_output.push_back(convert(ffi_output[index]));
    }
    if (!compare_state(
            cpp,
            rust,
            cpp_output,
            rust_output,
            cpp_error,
            rust_error,
            cpp_result,
            rust_result,
            "lifecycle after reset")) {
        return false;
    }

    cpp_result = cpp.finish(&cpp_output, &cpp_error);
    rust_result = rust.finish(ffi_output, &written, &rust_error);
    if (!compare_state(
            cpp,
            rust,
            cpp_output,
            rust_output,
            cpp_error,
            rust_error,
            cpp_result,
            rust_result,
            "lifecycle finish")) {
        return false;
    }

    cpp_result = cpp.finish(&cpp_output, &cpp_error);
    rust_result = rust.finish(ffi_output, &written, &rust_error);
    if (!compare_state(
            cpp,
            rust,
            cpp_output,
            rust_output,
            cpp_error,
            rust_error,
            cpp_result,
            rust_result,
            "lifecycle idempotent finish")) {
        return false;
    }

    cpp_result = cpp.feed(second, 1U, &cpp_output, &cpp_error);
    rust_result = rust.feed(second, 1U, ffi_output, &written, &rust_error);
    return compare_state(
        cpp,
        rust,
        cpp_output,
        rust_output,
        cpp_error,
        rust_error,
        cpp_result,
        rust_result,
        "lifecycle feed after finish");
}

bool test_source_overflow_equivalence() {
    return run_case(
        bytes({0x41U}),
        1U,
        std::numeric_limits<std::uint64_t>::max(),
        Utf8ErrorPolicy::Strict,
        false,
        "source range overflow");
}

bool test_rust_output_budget() {
    RustUtf8StreamDecoder decoder(Utf8ErrorPolicy::Strict);
    Utf8DecodeError error;
    std::size_t written = 99U;
    const auto input = bytes({0x41U});
    const bool result = decoder.feed(
        input,
        0U,
        std::span<ZrDecodedCodePoint>{},
        &written,
        &error);
    return require(!result, "Rust output budget rejects emission") &&
           require(written == 0U, "Rust output budget writes no records") &&
           require(
               error.kind == Utf8ErrorKind::OutputBudgetExceeded,
               "Rust output budget error kind") &&
           require(error.source_offset == 0U, "Rust output budget error offset") &&
           require(decoder.failed(), "Rust output budget latches failed state") &&
           require(
               decoder.stats().emitted_codepoints == 0U,
               "Rust output budget does not count rejected emission");
}

bool test_abi_contract() {
    return require(
               RustUtf8StreamDecoder::abi_version() == ZR_UTF8_ABI_VERSION,
               "UTF-8 ABI version") &&
           require(
               RustUtf8StreamDecoder::storage_size() ==
                   ZR_UTF8_DECODER_STORAGE_BYTES,
               "UTF-8 storage size") &&
           require(
               RustUtf8StreamDecoder::storage_alignment() ==
                   ZR_UTF8_DECODER_STORAGE_ALIGN,
               "UTF-8 storage alignment") &&
           require(sizeof(ZrDecodedCodePoint) == 16U, "UTF-8 output record size") &&
           require(sizeof(ZrUtf8DecodeStats) == 48U, "UTF-8 stats record size") &&
           require(sizeof(ZrUtf8DecodeError) == 16U, "UTF-8 error record size");
}

} // namespace

int main() {
    if (!test_abi_contract() ||
        !test_valid_chunk_equivalence() ||
        !test_strict_error_equivalence() ||
        !test_replacement_equivalence() ||
        !test_lifecycle_equivalence() ||
        !test_source_overflow_equivalence() ||
        !test_rust_output_budget()) {
        return 1;
    }

    std::cout << "Rust UTF-8 stream equivalence tests passed\n";
    return 0;
}
