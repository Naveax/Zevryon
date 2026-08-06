#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

#if defined(ZEVRYON_UTF8_RUST_SHADOW)
#include "zevryon_rust_ffi.h"
#endif

namespace zevryon::text {

enum class Utf8ErrorPolicy : std::uint8_t {
    Strict = 0,
    Replace
};

enum class Utf8ErrorKind : std::uint8_t {
    None = 0,
    DiscontinuousInput,
    InvalidLeadByte,
    UnexpectedContinuation,
    InvalidContinuation,
    OverlongEncoding,
    SurrogateCodePoint,
    CodePointOutOfRange,
    TruncatedSequence,
    OutputBudgetExceeded
};

const char* utf8_error_kind_name(Utf8ErrorKind kind) noexcept;

struct Utf8DecodeError {
    Utf8ErrorKind kind{Utf8ErrorKind::None};
    std::uint64_t source_offset{0};
    std::string message;
};

struct DecodedCodePoint {
    std::uint64_t source_start{0};
    std::uint32_t value{0};
    std::uint8_t source_length{0};
    bool replacement{false};

    constexpr DecodedCodePoint() noexcept = default;
    constexpr DecodedCodePoint(
        std::uint32_t input_value,
        std::uint64_t input_source_start,
        std::uint64_t input_source_end,
        bool input_replacement) noexcept
        : source_start(input_source_start),
          value(input_value),
          source_length(
              input_source_end >= input_source_start &&
                      input_source_end - input_source_start <= 255U
                  ? static_cast<std::uint8_t>(
                        input_source_end - input_source_start)
                  : 0U),
          replacement(input_replacement) {}

    constexpr std::uint64_t source_end() const noexcept {
        return source_start + static_cast<std::uint64_t>(source_length);
    }

    bool operator==(const DecodedCodePoint&) const noexcept = default;
};

static_assert(
    sizeof(DecodedCodePoint) <= 16U,
    "decoded code point records must remain within the Z1 memory contract");

struct Utf8DecodeStats {
    std::uint64_t source_bytes{0};
    std::uint64_t emitted_codepoints{0};
    std::uint64_t invalid_sequences{0};
    std::uint64_t replacements{0};
    std::uint64_t chunks{0};
    std::uint8_t maximum_pending_continuations{0};
};

class Utf8StreamDecoder {
public:
    explicit Utf8StreamDecoder(Utf8ErrorPolicy policy = Utf8ErrorPolicy::Strict) noexcept;
    ~Utf8StreamDecoder();

    bool feed(
        std::span<const std::byte> bytes,
        std::uint64_t absolute_source_offset,
        std::pmr::vector<DecodedCodePoint>* output,
        Utf8DecodeError* error) noexcept;

    bool finish(
        std::pmr::vector<DecodedCodePoint>* output,
        Utf8DecodeError* error) noexcept;

    void reset() noexcept;
    Utf8ErrorPolicy policy() const noexcept;
    const Utf8DecodeStats& stats() const noexcept;
    std::uint64_t next_source_offset() const noexcept;
    bool failed() const noexcept;

    bool rust_shadow_enabled() const noexcept;
    bool rust_shadow_healthy() const noexcept;
    std::uint64_t rust_shadow_operations() const noexcept;
    std::uint64_t rust_shadow_verifications() const noexcept;
    std::uint64_t rust_shadow_mismatches() const noexcept;
    std::string rust_shadow_json() const;

private:
    bool feed_cpp(
        std::span<const std::byte> bytes,
        std::uint64_t absolute_source_offset,
        std::pmr::vector<DecodedCodePoint>* output,
        Utf8DecodeError* error) noexcept;
    bool finish_cpp(
        std::pmr::vector<DecodedCodePoint>* output,
        Utf8DecodeError* error) noexcept;

    bool emit(
        std::uint32_t value,
        std::uint64_t source_start,
        std::uint64_t source_end,
        bool replacement,
        std::pmr::vector<DecodedCodePoint>* output,
        Utf8DecodeError* error) noexcept;
    bool fail(
        Utf8ErrorKind kind,
        std::uint64_t source_offset,
        const char* message,
        Utf8DecodeError* error) noexcept;
    bool handle_invalid_sequence(
        Utf8ErrorKind kind,
        std::uint64_t error_offset,
        std::uint64_t replacement_end,
        const char* message,
        std::pmr::vector<DecodedCodePoint>* output,
        Utf8DecodeError* error) noexcept;
    void start_sequence(
        std::uint8_t expected_continuations,
        std::uint32_t accumulator,
        std::uint32_t minimum_value,
        std::uint64_t source_start) noexcept;
    void clear_sequence() noexcept;

#if defined(ZEVRYON_UTF8_RUST_SHADOW)
    enum class RustShadowMismatchKind : std::uint8_t {
        None = 0,
        RustUnavailable,
        AbiVersion,
        StorageContract,
        OperationResult,
        OutputCount,
        OutputRecord,
        ErrorKind,
        ErrorOffset,
        Statistics,
        NextSourceOffset,
        FailedState,
        Policy,
        BufferAllocation,
        ResetResult,
    };

    void rust_shadow_feed(
        std::span<const std::byte> bytes,
        std::uint64_t absolute_source_offset,
        const std::pmr::vector<DecodedCodePoint>& output,
        std::size_t output_start,
        bool primary_result,
        const Utf8DecodeError& primary_error) noexcept;
    void rust_shadow_finish(
        const std::pmr::vector<DecodedCodePoint>& output,
        std::size_t output_start,
        bool primary_result,
        const Utf8DecodeError& primary_error) noexcept;
    void rust_shadow_reset() noexcept;
    bool rust_shadow_verify_state() noexcept;
    void rust_shadow_record_mismatch(
        RustShadowMismatchKind kind,
        std::uint64_t index,
        std::uint64_t expected,
        std::uint64_t actual) noexcept;
    static void increment_saturating(std::uint64_t& value) noexcept;

    ZrUtf8DecoderStorage rust_shadow_storage_{};
    std::vector<ZrDecodedCodePoint> rust_shadow_output_{};
    std::uint64_t rust_shadow_operations_{0};
    std::uint64_t rust_shadow_verifications_{0};
    std::uint64_t rust_shadow_mismatches_{0};
    std::uint64_t rust_shadow_first_index_{0};
    std::uint64_t rust_shadow_expected_{0};
    std::uint64_t rust_shadow_actual_{0};
    RustShadowMismatchKind rust_shadow_first_mismatch_{
        RustShadowMismatchKind::None};
    bool rust_shadow_initialized_{false};
#endif

    Utf8ErrorPolicy policy_;
    Utf8DecodeStats stats_;
    bool started_{false};
    bool finished_{false};
    bool failed_{false};
    std::uint64_t next_source_offset_{0};
    std::uint64_t sequence_start_{0};
    std::uint32_t accumulator_{0};
    std::uint32_t minimum_value_{0};
    std::uint8_t pending_continuations_{0};
};

#if defined(ZEVRYON_UTF8_RUST_SHADOW)
static_assert(sizeof(ZrDecodedCodePoint) == 16U);
static_assert(sizeof(ZrUtf8DecodeStats) == 48U);
static_assert(sizeof(ZrUtf8DecodeError) == 16U);
static_assert(sizeof(ZrUtf8DecoderStorage) == ZR_UTF8_DECODER_STORAGE_BYTES);
static_assert(alignof(ZrUtf8DecoderStorage) == ZR_UTF8_DECODER_STORAGE_ALIGN);
#endif

} // namespace zevryon::text
