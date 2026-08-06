#include "unicode_stream.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

namespace {

using zevryon::text::DecodedCodePoint;
using zevryon::text::Utf8DecodeError;
using zevryon::text::Utf8ErrorKind;
using zevryon::text::Utf8ErrorPolicy;
using zevryon::text::Utf8StreamDecoder;

[[noreturn]] void fail(const char* message) {
    std::fprintf(stderr, "unicode authority test failed: %s\n", message);
    std::abort();
}

void require(bool condition, const char* message) {
    if (!condition) {
        fail(message);
    }
}

void set_fault(std::string_view fault) {
#if defined(_WIN32)
    const int result = _putenv_s(
        "ZEVRYON_UTF8_AUTHORITY_CPP_FAULT",
        std::string(fault).c_str());
#else
    const int result = setenv(
        "ZEVRYON_UTF8_AUTHORITY_CPP_FAULT",
        std::string(fault).c_str(),
        1);
#endif
    require(result == 0, "failed to set fault environment");
}

void clear_fault() {
#if defined(_WIN32)
    const int result = _putenv_s("ZEVRYON_UTF8_AUTHORITY_CPP_FAULT", "");
#else
    const int result = unsetenv("ZEVRYON_UTF8_AUTHORITY_CPP_FAULT");
#endif
    require(result == 0, "failed to clear fault environment");
}

void require_authority_identity(const Utf8StreamDecoder& decoder) {
    const std::string telemetry = decoder.rust_shadow_json();
    require(
        telemetry.find("\"schema\":\"zevryon.rust-unicode-authority.v1\"") !=
            std::string::npos,
        "authority schema missing");
    require(
        telemetry.find("\"authoritative_backend\":\"rust\"") !=
            std::string::npos,
        "Rust authority identity missing");
    require(
        telemetry.find("\"reverse_shadow_backend\":\"cpp\"") !=
            std::string::npos,
        "C++ reverse-shadow identity missing");
    require(
        telemetry.find("\"fallback_permitted\":false") !=
            std::string::npos,
        "fallback contract missing");
}

void require_error(
    const Utf8DecodeError& error,
    Utf8ErrorKind kind,
    std::uint64_t source_offset,
    std::string_view message,
    const char* context) {
    require(error.kind == kind, context);
    require(error.source_offset == source_offset, context);
    require(error.message == message, context);
}

void positive_authority_round_trip() {
    Utf8StreamDecoder decoder(Utf8ErrorPolicy::Strict);
    std::pmr::vector<DecodedCodePoint> output;
    Utf8DecodeError error;

    const std::array<std::byte, 3> first = {
        std::byte{0x41}, std::byte{0xc5}, std::byte{0x9f}};
    const std::array<std::byte, 4> second = {
        std::byte{0xf0}, std::byte{0x9f}, std::byte{0x98}, std::byte{0x80}};

    require(decoder.feed(first, 100U, &output, &error), "first feed failed");
    require(error.kind == Utf8ErrorKind::None, "first feed returned an error");
    require(error.message.empty(), "first feed returned an error message");
    require(decoder.feed(second, 103U, &output, &error), "second feed failed");
    require(decoder.finish(&output, &error), "finish failed");
    require(error.kind == Utf8ErrorKind::None, "finish returned an error");
    require(error.message.empty(), "finish returned an error message");

    require(output.size() == 3U, "unexpected decoded output count");
    require(output[0].value == 0x41U, "ASCII output mismatch");
    require(output[0].source_start == 100U, "ASCII source offset mismatch");
    require(output[1].value == 0x15fU, "two-byte output mismatch");
    require(output[1].source_start == 101U, "two-byte source offset mismatch");
    require(output[2].value == 0x1f600U, "four-byte output mismatch");
    require(output[2].source_start == 103U, "four-byte source offset mismatch");

    require(decoder.stats().source_bytes == 7U, "source-byte statistic mismatch");
    require(
        decoder.stats().emitted_codepoints == 3U,
        "emitted-codepoint statistic mismatch");
    require(decoder.stats().chunks == 2U, "chunk statistic mismatch");
    require(decoder.next_source_offset() == 107U, "next source offset mismatch");
    require(!decoder.failed(), "positive decoder entered failed state");
    require(decoder.rust_shadow_enabled(), "Rust authority is not enabled");
    require(decoder.rust_shadow_healthy(), "positive reverse shadow is unhealthy");
    require(decoder.rust_shadow_mismatches() == 0U, "positive mismatch detected");
    require_authority_identity(decoder);
}

void replacement_and_strict_error_are_rust_public_results() {
    const std::array<std::byte, 1> invalid = {std::byte{0xff}};

    Utf8StreamDecoder replace(Utf8ErrorPolicy::Replace);
    std::pmr::vector<DecodedCodePoint> replaced;
    Utf8DecodeError replace_error;
    require(replace.feed(invalid, 9U, &replaced, &replace_error), "replace feed failed");
    require(replace.finish(&replaced, &replace_error), "replace finish failed");
    require(replaced.size() == 1U, "replacement output count mismatch");
    require(replaced[0].value == 0xfffdU, "replacement code point mismatch");
    require(replaced[0].replacement, "replacement flag missing");
    require(replace.stats().invalid_sequences == 1U, "invalid statistic mismatch");
    require(replace.stats().replacements == 1U, "replacement statistic mismatch");
    require(replace.rust_shadow_healthy(), "replacement reverse shadow mismatch");

    Utf8StreamDecoder strict(Utf8ErrorPolicy::Strict);
    std::pmr::vector<DecodedCodePoint> strict_output;
    Utf8DecodeError strict_error;
    require(!strict.feed(invalid, 44U, &strict_output, &strict_error), "strict feed succeeded");
    require_error(
        strict_error,
        Utf8ErrorKind::InvalidLeadByte,
        44U,
        "invalid UTF-8 lead byte",
        "strict invalid-lead error mismatch");
    require(strict.failed(), "strict decoder did not enter failed state");
    require(strict_output.empty(), "strict decoder emitted output");
    require(strict.rust_shadow_healthy(), "strict reverse shadow mismatch");
}

void exact_error_messages_match_cpp_contract() {
    const std::array<std::byte, 1> first = {std::byte{0x41}};
    const std::array<std::byte, 1> second = {std::byte{0x42}};

    Utf8StreamDecoder lifecycle(Utf8ErrorPolicy::Strict);
    std::pmr::vector<DecodedCodePoint> output;
    Utf8DecodeError error;
    require(lifecycle.feed(first, 10U, &output, &error), "lifecycle first feed failed");
    require(!lifecycle.feed(second, 12U, &output, &error), "discontinuous feed succeeded");
    require_error(
        error,
        Utf8ErrorKind::DiscontinuousInput,
        12U,
        "UTF-8 input chunks are not contiguous",
        "discontinuous-offset message mismatch");
    require(!lifecycle.feed(second, 11U, &output, &error), "failed-state feed succeeded");
    require_error(
        error,
        Utf8ErrorKind::DiscontinuousInput,
        11U,
        "UTF-8 decoder is in a failed state",
        "failed-state message mismatch");

    lifecycle.reset();
    output.clear();
    require(lifecycle.feed(first, 0U, &output, &error), "post-reset feed failed");
    require(lifecycle.finish(&output, &error), "post-reset finish failed");
    require(!lifecycle.feed(second, 1U, &output, &error), "post-finish feed succeeded");
    require_error(
        error,
        Utf8ErrorKind::DiscontinuousInput,
        1U,
        "UTF-8 decoder already finished",
        "finished-state message mismatch");

    Utf8StreamDecoder overflow(Utf8ErrorPolicy::Strict);
    std::pmr::vector<DecodedCodePoint> overflow_output;
    Utf8DecodeError overflow_error;
    require(
        !overflow.feed(
            first,
            std::numeric_limits<std::uint64_t>::max(),
            &overflow_output,
            &overflow_error),
        "overflow feed succeeded");
    require_error(
        overflow_error,
        Utf8ErrorKind::DiscontinuousInput,
        std::numeric_limits<std::uint64_t>::max(),
        "UTF-8 source range overflows 64-bit offsets",
        "source-range message mismatch");

    require(lifecycle.rust_shadow_healthy(), "lifecycle message reverse shadow mismatch");
    require(overflow.rust_shadow_healthy(), "overflow message reverse shadow mismatch");
}

void output_budget_is_fail_closed_without_cpp_fallback() {
    Utf8StreamDecoder decoder(Utf8ErrorPolicy::Strict);
    std::pmr::vector<DecodedCodePoint> output(
        std::pmr::null_memory_resource());
    Utf8DecodeError error;
    const std::array<std::byte, 1> input = {std::byte{0x41}};

    require(!decoder.feed(input, 0U, &output, &error), "budget feed succeeded");
    require_error(
        error,
        Utf8ErrorKind::OutputBudgetExceeded,
        0U,
        "UTF-8 output exceeded its resource budget",
        "budget error contract mismatch");
    require(output.empty(), "budget failure emitted output");
    require(decoder.failed(), "budget failure did not latch failed state");
    require(decoder.rust_shadow_healthy(), "budget reverse shadow mismatch");
    require_authority_identity(decoder);
}

void positive() {
    clear_fault();
    positive_authority_round_trip();
    replacement_and_strict_error_are_rust_public_results();
    exact_error_messages_match_cpp_contract();
    output_budget_is_fail_closed_without_cpp_fallback();
}

void fault(std::string_view selected) {
    set_fault(selected);
    Utf8StreamDecoder decoder(
        selected == "error" ? Utf8ErrorPolicy::Strict
                            : Utf8ErrorPolicy::Replace);
    std::pmr::vector<DecodedCodePoint> output;
    Utf8DecodeError error;

    if (selected == "reset") {
        decoder.reset();
    } else if (selected == "error") {
        const std::array<std::byte, 1> input = {std::byte{0xff}};
        require(!decoder.feed(input, 0U, &output, &error), "error fault feed succeeded");
        require(
            error.kind == Utf8ErrorKind::InvalidLeadByte,
            "error fault changed Rust public error");
        require(
            error.message == "invalid UTF-8 lead byte",
            "error fault changed Rust public message");
        require(output.empty(), "error fault changed Rust public output");
    } else {
        const std::array<std::byte, 1> input = {std::byte{0x41}};
        require(decoder.feed(input, 0U, &output, &error), "fault feed failed");
        require(output.size() == 1U, "fault changed public output count");
        require(output[0].value == 0x41U, "fault changed Rust public output");
        require(error.kind == Utf8ErrorKind::None, "fault changed Rust public error");
        require(error.message.empty(), "fault changed Rust public message");
    }

    require(decoder.rust_shadow_enabled(), "authority disabled during fault test");
    require(!decoder.rust_shadow_healthy(), "fault was not detected");
    require(decoder.rust_shadow_mismatches() != 0U, "fault mismatch count is zero");
    require_authority_identity(decoder);

    const std::string telemetry = decoder.rust_shadow_json();
    if (selected == "output") {
        require(
            telemetry.find("\"first_mismatch\":\"output_record\"") !=
                std::string::npos,
            "output fault classification mismatch");
    } else if (selected == "error") {
        require(
            telemetry.find("\"first_mismatch\":\"error_kind\"") !=
                std::string::npos,
            "error fault classification mismatch");
    } else if (selected == "state") {
        require(
            telemetry.find("\"first_mismatch\":\"statistics\"") !=
                std::string::npos,
            "state fault classification mismatch");
    } else if (selected == "reset") {
        require(
            telemetry.find("\"first_mismatch\":\"reset_result\"") !=
                std::string::npos,
            "reset fault classification mismatch");
    } else {
        fail("unknown fault");
    }
    clear_fault();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fail("missing mode");
    }
    const std::string_view mode(argv[1]);
    if (mode == "--positive") {
        positive();
        return 0;
    }
    if (mode == "--fault") {
        if (argc != 3) {
            fail("fault mode requires one fault name");
        }
        fault(argv[2]);
        return 0;
    }
    fail("unknown mode");
}
