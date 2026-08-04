#include "unicode_stream.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <new>
#include <sstream>

namespace zevryon::text {
namespace {

constexpr std::uint32_t kReplacementCharacter = 0xfffdU;

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

void clear_error(Utf8DecodeError* error) noexcept {
    if (error != nullptr) {
        error->kind = Utf8ErrorKind::None;
        error->source_offset = 0U;
        error->message.clear();
    }
}

} // namespace

const char* utf8_error_kind_name(Utf8ErrorKind kind) noexcept {
    switch (kind) {
        case Utf8ErrorKind::None:
            return "none";
        case Utf8ErrorKind::DiscontinuousInput:
            return "discontinuous_input";
        case Utf8ErrorKind::InvalidLeadByte:
            return "invalid_lead_byte";
        case Utf8ErrorKind::UnexpectedContinuation:
            return "unexpected_continuation";
        case Utf8ErrorKind::InvalidContinuation:
            return "invalid_continuation";
        case Utf8ErrorKind::OverlongEncoding:
            return "overlong_encoding";
        case Utf8ErrorKind::SurrogateCodePoint:
            return "surrogate_code_point";
        case Utf8ErrorKind::CodePointOutOfRange:
            return "code_point_out_of_range";
        case Utf8ErrorKind::TruncatedSequence:
            return "truncated_sequence";
        case Utf8ErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
    }
    return "invalid";
}

Utf8StreamDecoder::Utf8StreamDecoder(Utf8ErrorPolicy policy) noexcept
    : policy_(policy) {
#if defined(ZEVRYON_UTF8_RUST_SHADOW)
    rust_shadow_initialized_ =
        zr_utf8_decoder_init(
            &rust_shadow_storage_,
            static_cast<std::uint32_t>(policy_)) != 0U;
    if (!rust_shadow_initialized_) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::RustUnavailable, 0U, 1U, 0U);
        return;
    }
    if (zr_utf8_abi_version() != ZR_UTF8_ABI_VERSION) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::AbiVersion,
            0U,
            ZR_UTF8_ABI_VERSION,
            zr_utf8_abi_version());
        rust_shadow_initialized_ = false;
        return;
    }
    if (zr_utf8_decoder_storage_size() != ZR_UTF8_DECODER_STORAGE_BYTES) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::StorageContract,
            0U,
            ZR_UTF8_DECODER_STORAGE_BYTES,
            zr_utf8_decoder_storage_size());
        rust_shadow_initialized_ = false;
        return;
    }
    if (zr_utf8_decoder_storage_alignment() != ZR_UTF8_DECODER_STORAGE_ALIGN) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::StorageContract,
            1U,
            ZR_UTF8_DECODER_STORAGE_ALIGN,
            zr_utf8_decoder_storage_alignment());
        rust_shadow_initialized_ = false;
        return;
    }
    static_cast<void>(rust_shadow_verify_state());
#endif
}

Utf8StreamDecoder::~Utf8StreamDecoder() {
#if defined(ZEVRYON_UTF8_RUST_SHADOW)
    if (rust_shadow_initialized_) {
        zr_utf8_decoder_clear(&rust_shadow_storage_);
    }
    rust_shadow_initialized_ = false;
#endif
}

bool Utf8StreamDecoder::feed(
    std::span<const std::byte> bytes,
    std::uint64_t absolute_source_offset,
    std::pmr::vector<DecodedCodePoint>* output,
    Utf8DecodeError* error) noexcept {
    const std::size_t output_start = output != nullptr ? output->size() : 0U;
    const bool primary_result =
        feed_cpp(bytes, absolute_source_offset, output, error);
#if defined(ZEVRYON_UTF8_RUST_SHADOW)
    if (output != nullptr && error != nullptr) {
        rust_shadow_feed(
            bytes,
            absolute_source_offset,
            *output,
            output_start,
            primary_result,
            *error);
    }
#endif
    return primary_result;
}

bool Utf8StreamDecoder::finish(
    std::pmr::vector<DecodedCodePoint>* output,
    Utf8DecodeError* error) noexcept {
    const std::size_t output_start = output != nullptr ? output->size() : 0U;
    const bool primary_result = finish_cpp(output, error);
#if defined(ZEVRYON_UTF8_RUST_SHADOW)
    if (output != nullptr && error != nullptr) {
        rust_shadow_finish(
            *output,
            output_start,
            primary_result,
            *error);
    }
#endif
    return primary_result;
}

bool Utf8StreamDecoder::feed_cpp(
    std::span<const std::byte> bytes,
    std::uint64_t absolute_source_offset,
    std::pmr::vector<DecodedCodePoint>* output,
    Utf8DecodeError* error) noexcept {
    if (output == nullptr || error == nullptr) {
        return false;
    }
    clear_error(error);
    if (failed_) {
        return fail(
            Utf8ErrorKind::DiscontinuousInput,
            absolute_source_offset,
            "UTF-8 decoder is in a failed state",
            error);
    }
    if (finished_) {
        return fail(
            Utf8ErrorKind::DiscontinuousInput,
            absolute_source_offset,
            "UTF-8 decoder already finished",
            error);
    }
    if (started_ && absolute_source_offset != next_source_offset_) {
        return fail(
            Utf8ErrorKind::DiscontinuousInput,
            absolute_source_offset,
            "UTF-8 input chunks are not contiguous",
            error);
    }
    if (bytes.size() >
        std::numeric_limits<std::uint64_t>::max() - absolute_source_offset) {
        return fail(
            Utf8ErrorKind::DiscontinuousInput,
            absolute_source_offset,
            "UTF-8 source range overflows 64-bit offsets",
            error);
    }

    if (!started_) {
        started_ = true;
        next_source_offset_ = absolute_source_offset;
    }
    ++stats_.chunks;
    stats_.source_bytes = saturating_add(
        stats_.source_bytes, static_cast<std::uint64_t>(bytes.size()));

    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        const std::uint64_t source_offset =
            absolute_source_offset + static_cast<std::uint64_t>(index);
        const std::uint8_t byte = static_cast<std::uint8_t>(
            std::to_integer<unsigned int>(bytes[index]));
        bool retry = true;
        while (retry) {
            retry = false;
            if (pending_continuations_ != 0U) {
                if ((byte & 0xc0U) == 0x80U) {
                    accumulator_ = static_cast<std::uint32_t>(
                        (accumulator_ << 6U) | (byte & 0x3fU));
                    --pending_continuations_;
                    if (pending_continuations_ == 0U) {
                        const std::uint64_t source_end = source_offset + 1U;
                        if (accumulator_ < minimum_value_) {
                            if (!handle_invalid_sequence(
                                    Utf8ErrorKind::OverlongEncoding,
                                    sequence_start_,
                                    source_end,
                                    "overlong UTF-8 sequence",
                                    output,
                                    error)) {
                                return false;
                            }
                        } else if (accumulator_ >= 0xd800U && accumulator_ <= 0xdfffU) {
                            if (!handle_invalid_sequence(
                                    Utf8ErrorKind::SurrogateCodePoint,
                                    sequence_start_,
                                    source_end,
                                    "UTF-8 sequence encodes a surrogate code point",
                                    output,
                                    error)) {
                                return false;
                            }
                        } else if (accumulator_ > 0x10ffffU) {
                            if (!handle_invalid_sequence(
                                    Utf8ErrorKind::CodePointOutOfRange,
                                    sequence_start_,
                                    source_end,
                                    "UTF-8 code point exceeds Unicode range",
                                    output,
                                    error)) {
                                return false;
                            }
                        } else {
                            const std::uint32_t value = accumulator_;
                            const std::uint64_t source_start = sequence_start_;
                            clear_sequence();
                            if (!emit(
                                    value,
                                    source_start,
                                    source_end,
                                    false,
                                    output,
                                    error)) {
                                return false;
                            }
                        }
                    }
                    continue;
                }

                if (!handle_invalid_sequence(
                        Utf8ErrorKind::InvalidContinuation,
                        source_offset,
                        source_offset,
                        "UTF-8 sequence contains a non-continuation byte",
                        output,
                        error)) {
                    return false;
                }
                retry = true;
                continue;
            }

            if (byte <= 0x7fU) {
                if (!emit(
                        byte,
                        source_offset,
                        source_offset + 1U,
                        false,
                        output,
                        error)) {
                    return false;
                }
            } else if (byte >= 0xc2U && byte <= 0xdfU) {
                start_sequence(1U, byte & 0x1fU, 0x80U, source_offset);
            } else if (byte >= 0xe0U && byte <= 0xefU) {
                start_sequence(2U, byte & 0x0fU, 0x800U, source_offset);
            } else if (byte >= 0xf0U && byte <= 0xf4U) {
                start_sequence(3U, byte & 0x07U, 0x10000U, source_offset);
            } else {
                sequence_start_ = source_offset;
                const Utf8ErrorKind kind =
                    (byte & 0xc0U) == 0x80U
                        ? Utf8ErrorKind::UnexpectedContinuation
                        : Utf8ErrorKind::InvalidLeadByte;
                const char* message =
                    kind == Utf8ErrorKind::UnexpectedContinuation
                        ? "unexpected UTF-8 continuation byte"
                        : "invalid UTF-8 lead byte";
                if (!handle_invalid_sequence(
                        kind,
                        source_offset,
                        source_offset + 1U,
                        message,
                        output,
                        error)) {
                    return false;
                }
            }
        }
    }

    next_source_offset_ =
        absolute_source_offset + static_cast<std::uint64_t>(bytes.size());
    return true;
}

bool Utf8StreamDecoder::finish_cpp(
    std::pmr::vector<DecodedCodePoint>* output,
    Utf8DecodeError* error) noexcept {
    if (output == nullptr || error == nullptr) {
        return false;
    }
    clear_error(error);
    if (failed_) {
        return fail(
            Utf8ErrorKind::DiscontinuousInput,
            next_source_offset_,
            "UTF-8 decoder is in a failed state",
            error);
    }
    if (finished_) {
        return true;
    }
    if (pending_continuations_ != 0U) {
        if (!handle_invalid_sequence(
                Utf8ErrorKind::TruncatedSequence,
                next_source_offset_,
                next_source_offset_,
                "UTF-8 input ended inside a sequence",
                output,
                error)) {
            return false;
        }
    }
    finished_ = true;
    return true;
}

void Utf8StreamDecoder::reset() noexcept {
    stats_ = {};
    started_ = false;
    finished_ = false;
    failed_ = false;
    next_source_offset_ = 0U;
    clear_sequence();
#if defined(ZEVRYON_UTF8_RUST_SHADOW)
    rust_shadow_reset();
#endif
}

Utf8ErrorPolicy Utf8StreamDecoder::policy() const noexcept {
    return policy_;
}

const Utf8DecodeStats& Utf8StreamDecoder::stats() const noexcept {
    return stats_;
}

std::uint64_t Utf8StreamDecoder::next_source_offset() const noexcept {
    return next_source_offset_;
}

bool Utf8StreamDecoder::failed() const noexcept {
    return failed_;
}

bool Utf8StreamDecoder::rust_shadow_enabled() const noexcept {
#if defined(ZEVRYON_UTF8_RUST_SHADOW)
    return rust_shadow_initialized_;
#else
    return false;
#endif
}

bool Utf8StreamDecoder::rust_shadow_healthy() const noexcept {
#if defined(ZEVRYON_UTF8_RUST_SHADOW)
    return rust_shadow_initialized_ && rust_shadow_mismatches_ == 0U;
#else
    return false;
#endif
}

std::uint64_t Utf8StreamDecoder::rust_shadow_operations() const noexcept {
#if defined(ZEVRYON_UTF8_RUST_SHADOW)
    return rust_shadow_operations_;
#else
    return 0U;
#endif
}

std::uint64_t Utf8StreamDecoder::rust_shadow_verifications() const noexcept {
#if defined(ZEVRYON_UTF8_RUST_SHADOW)
    return rust_shadow_verifications_;
#else
    return 0U;
#endif
}

std::uint64_t Utf8StreamDecoder::rust_shadow_mismatches() const noexcept {
#if defined(ZEVRYON_UTF8_RUST_SHADOW)
    return rust_shadow_mismatches_;
#else
    return 0U;
#endif
}

std::string Utf8StreamDecoder::rust_shadow_json() const {
#if defined(ZEVRYON_UTF8_RUST_SHADOW)
    const char* mismatch_name = "none";
    switch (rust_shadow_first_mismatch_) {
        case RustShadowMismatchKind::None:
            break;
        case RustShadowMismatchKind::RustUnavailable:
            mismatch_name = "rust_unavailable";
            break;
        case RustShadowMismatchKind::AbiVersion:
            mismatch_name = "abi_version";
            break;
        case RustShadowMismatchKind::StorageContract:
            mismatch_name = "storage_contract";
            break;
        case RustShadowMismatchKind::OperationResult:
            mismatch_name = "operation_result";
            break;
        case RustShadowMismatchKind::OutputCount:
            mismatch_name = "output_count";
            break;
        case RustShadowMismatchKind::OutputRecord:
            mismatch_name = "output_record";
            break;
        case RustShadowMismatchKind::ErrorKind:
            mismatch_name = "error_kind";
            break;
        case RustShadowMismatchKind::ErrorOffset:
            mismatch_name = "error_offset";
            break;
        case RustShadowMismatchKind::Statistics:
            mismatch_name = "statistics";
            break;
        case RustShadowMismatchKind::NextSourceOffset:
            mismatch_name = "next_source_offset";
            break;
        case RustShadowMismatchKind::FailedState:
            mismatch_name = "failed_state";
            break;
        case RustShadowMismatchKind::Policy:
            mismatch_name = "policy";
            break;
        case RustShadowMismatchKind::BufferAllocation:
            mismatch_name = "buffer_allocation";
            break;
        case RustShadowMismatchKind::ResetResult:
            mismatch_name = "reset_result";
            break;
    }

    std::ostringstream output;
    output << "{\"schema\":\"zevryon.rust-unicode-shadow.v1\","
           << "\"enabled\":"
           << (rust_shadow_initialized_ ? "true" : "false") << ','
           << "\"strict\":"
           << (ZEVRYON_RUST_UNICODE_SHADOW_STRICT != 0 ? "true" : "false") << ','
           << "\"abi_version\":" << zr_utf8_abi_version() << ','
           << "\"operations\":" << rust_shadow_operations_ << ','
           << "\"verifications\":" << rust_shadow_verifications_ << ','
           << "\"mismatches\":" << rust_shadow_mismatches_ << ','
           << "\"healthy\":"
           << (rust_shadow_healthy() ? "true" : "false") << ','
           << "\"first_mismatch\":\"" << mismatch_name << "\","
           << "\"first_index\":" << rust_shadow_first_index_ << ','
           << "\"expected\":" << rust_shadow_expected_ << ','
           << "\"actual\":" << rust_shadow_actual_ << '}';
    return output.str();
#else
    return "{\"schema\":\"zevryon.rust-unicode-shadow.v1\","
           "\"enabled\":false,\"strict\":false,\"abi_version\":0,"
           "\"operations\":0,\"verifications\":0,\"mismatches\":0,"
           "\"healthy\":false,\"first_mismatch\":\"none\","
           "\"first_index\":0,\"expected\":0,\"actual\":0}";
#endif
}

bool Utf8StreamDecoder::emit(
    std::uint32_t value,
    std::uint64_t source_start,
    std::uint64_t source_end,
    bool replacement,
    std::pmr::vector<DecodedCodePoint>* output,
    Utf8DecodeError* error) noexcept {
    try {
        output->push_back({value, source_start, source_end, replacement});
    } catch (const std::bad_alloc&) {
        return fail(
            Utf8ErrorKind::OutputBudgetExceeded,
            source_start,
            "UTF-8 output exceeded its resource budget",
            error);
    } catch (...) {
        return fail(
            Utf8ErrorKind::OutputBudgetExceeded,
            source_start,
            "UTF-8 output allocation failed",
            error);
    }
    ++stats_.emitted_codepoints;
    if (replacement) {
        ++stats_.replacements;
    }
    return true;
}

bool Utf8StreamDecoder::fail(
    Utf8ErrorKind kind,
    std::uint64_t source_offset,
    const char* message,
    Utf8DecodeError* error) noexcept {
    failed_ = true;
    if (error != nullptr) {
        error->kind = kind;
        error->source_offset = source_offset;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool Utf8StreamDecoder::handle_invalid_sequence(
    Utf8ErrorKind kind,
    std::uint64_t error_offset,
    std::uint64_t replacement_end,
    const char* message,
    std::pmr::vector<DecodedCodePoint>* output,
    Utf8DecodeError* error) noexcept {
    ++stats_.invalid_sequences;
    if (policy_ == Utf8ErrorPolicy::Strict) {
        return fail(kind, error_offset, message, error);
    }
    const std::uint64_t replacement_start = sequence_start_;
    clear_sequence();
    return emit(
        kReplacementCharacter,
        replacement_start,
        replacement_end,
        true,
        output,
        error);
}

void Utf8StreamDecoder::start_sequence(
    std::uint8_t expected_continuations,
    std::uint32_t accumulator,
    std::uint32_t minimum_value,
    std::uint64_t source_start) noexcept {
    pending_continuations_ = expected_continuations;
    accumulator_ = accumulator;
    minimum_value_ = minimum_value;
    sequence_start_ = source_start;
    stats_.maximum_pending_continuations = std::max(
        stats_.maximum_pending_continuations,
        pending_continuations_);
}

void Utf8StreamDecoder::clear_sequence() noexcept {
    sequence_start_ = 0U;
    accumulator_ = 0U;
    minimum_value_ = 0U;
    pending_continuations_ = 0U;
}

#if defined(ZEVRYON_UTF8_RUST_SHADOW)
void Utf8StreamDecoder::rust_shadow_feed(
    std::span<const std::byte> bytes,
    std::uint64_t absolute_source_offset,
    const std::pmr::vector<DecodedCodePoint>& output,
    std::size_t output_start,
    bool primary_result,
    const Utf8DecodeError& primary_error) noexcept {
    increment_saturating(rust_shadow_operations_);
    if (!rust_shadow_initialized_) {
        return;
    }

    const std::size_t appended =
        output.size() >= output_start ? output.size() - output_start : 0U;
    std::size_t capacity = 0U;
    if (!primary_result &&
        primary_error.kind == Utf8ErrorKind::OutputBudgetExceeded) {
        capacity = appended;
    } else if (bytes.size() == std::numeric_limits<std::size_t>::max()) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::BufferAllocation,
            0U,
            0U,
            std::numeric_limits<std::uint64_t>::max());
        return;
    } else {
        capacity = bytes.size() + 1U;
    }

    try {
        rust_shadow_output_.resize(capacity);
    } catch (...) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::BufferAllocation,
            0U,
            static_cast<std::uint64_t>(capacity),
            0U);
        return;
    }

    std::size_t written = 0U;
    ZrUtf8DecodeError shadow_error{};
    const auto* input = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const bool shadow_result =
        zr_utf8_decoder_feed(
            &rust_shadow_storage_,
            input,
            bytes.size(),
            absolute_source_offset,
            rust_shadow_output_.data(),
            capacity,
            &written,
            &shadow_error) != 0U;

    if (shadow_result != primary_result) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::OperationResult,
            0U,
            primary_result ? 1U : 0U,
            shadow_result ? 1U : 0U);
    }
    if (written != appended) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::OutputCount,
            0U,
            static_cast<std::uint64_t>(appended),
            static_cast<std::uint64_t>(written));
    }

    const std::size_t comparable = std::min(written, appended);
    for (std::size_t index = 0U; index < comparable; ++index) {
        const DecodedCodePoint& primary = output[output_start + index];
        const ZrDecodedCodePoint& shadow = rust_shadow_output_[index];
        const std::uint64_t record_index =
            static_cast<std::uint64_t>(index) * 4U;
        if (primary.source_start != shadow.source_start) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::OutputRecord,
                record_index,
                primary.source_start,
                shadow.source_start);
        }
        if (primary.value != shadow.value) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::OutputRecord,
                record_index + 1U,
                primary.value,
                shadow.value);
        }
        if (primary.source_length != shadow.source_length) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::OutputRecord,
                record_index + 2U,
                primary.source_length,
                shadow.source_length);
        }
        const std::uint64_t primary_replacement =
            primary.replacement ? 1U : 0U;
        if (primary_replacement != shadow.replacement) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::OutputRecord,
                record_index + 3U,
                primary_replacement,
                shadow.replacement);
        }
    }

    const std::uint64_t primary_error_kind =
        static_cast<std::uint64_t>(primary_error.kind);
    if (primary_error_kind != shadow_error.kind) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::ErrorKind,
            0U,
            primary_error_kind,
            shadow_error.kind);
    }
    if (primary_error.source_offset != shadow_error.source_offset) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::ErrorOffset,
            0U,
            primary_error.source_offset,
            shadow_error.source_offset);
    }
    static_cast<void>(rust_shadow_verify_state());
}

void Utf8StreamDecoder::rust_shadow_finish(
    const std::pmr::vector<DecodedCodePoint>& output,
    std::size_t output_start,
    bool primary_result,
    const Utf8DecodeError& primary_error) noexcept {
    increment_saturating(rust_shadow_operations_);
    if (!rust_shadow_initialized_) {
        return;
    }

    const std::size_t appended =
        output.size() >= output_start ? output.size() - output_start : 0U;
    const std::size_t capacity =
        !primary_result &&
                primary_error.kind == Utf8ErrorKind::OutputBudgetExceeded
            ? appended
            : 1U;
    try {
        rust_shadow_output_.resize(capacity);
    } catch (...) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::BufferAllocation,
            0U,
            static_cast<std::uint64_t>(capacity),
            0U);
        return;
    }

    std::size_t written = 0U;
    ZrUtf8DecodeError shadow_error{};
    const bool shadow_result =
        zr_utf8_decoder_finish(
            &rust_shadow_storage_,
            rust_shadow_output_.data(),
            capacity,
            &written,
            &shadow_error) != 0U;

    if (shadow_result != primary_result) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::OperationResult,
            0U,
            primary_result ? 1U : 0U,
            shadow_result ? 1U : 0U);
    }
    if (written != appended) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::OutputCount,
            0U,
            static_cast<std::uint64_t>(appended),
            static_cast<std::uint64_t>(written));
    }
    if (written != 0U && appended != 0U) {
        const DecodedCodePoint& primary = output[output_start];
        const ZrDecodedCodePoint& shadow = rust_shadow_output_[0];
        if (primary.source_start != shadow.source_start) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::OutputRecord,
                0U,
                primary.source_start,
                shadow.source_start);
        }
        if (primary.value != shadow.value) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::OutputRecord,
                1U,
                primary.value,
                shadow.value);
        }
        if (primary.source_length != shadow.source_length) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::OutputRecord,
                2U,
                primary.source_length,
                shadow.source_length);
        }
        const std::uint64_t primary_replacement =
            primary.replacement ? 1U : 0U;
        if (primary_replacement != shadow.replacement) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::OutputRecord,
                3U,
                primary_replacement,
                shadow.replacement);
        }
    }

    const std::uint64_t primary_error_kind =
        static_cast<std::uint64_t>(primary_error.kind);
    if (primary_error_kind != shadow_error.kind) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::ErrorKind,
            0U,
            primary_error_kind,
            shadow_error.kind);
    }
    if (primary_error.source_offset != shadow_error.source_offset) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::ErrorOffset,
            0U,
            primary_error.source_offset,
            shadow_error.source_offset);
    }
    static_cast<void>(rust_shadow_verify_state());
}

void Utf8StreamDecoder::rust_shadow_reset() noexcept {
    increment_saturating(rust_shadow_operations_);
    rust_shadow_output_.clear();
    if (!rust_shadow_initialized_) {
        return;
    }
    if (zr_utf8_decoder_reset(&rust_shadow_storage_) == 0U) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::ResetResult, 0U, 1U, 0U);
    }
    static_cast<void>(rust_shadow_verify_state());
}

bool Utf8StreamDecoder::rust_shadow_verify_state() noexcept {
    increment_saturating(rust_shadow_verifications_);
    if (!rust_shadow_initialized_) {
        return false;
    }

    bool matches = true;
    const auto compare = [this, &matches](
                             std::uint64_t index,
                             std::uint64_t expected,
                             std::uint64_t actual) noexcept {
        if (expected != actual) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::Statistics,
                index,
                expected,
                actual);
            matches = false;
        }
    };

    ZrUtf8DecodeStats shadow_stats{};
    if (zr_utf8_decoder_stats(&rust_shadow_storage_, &shadow_stats) == 0U) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::RustUnavailable, 1U, 1U, 0U);
        return false;
    }
    compare(0U, stats_.source_bytes, shadow_stats.source_bytes);
    compare(1U, stats_.emitted_codepoints, shadow_stats.emitted_codepoints);
    compare(2U, stats_.invalid_sequences, shadow_stats.invalid_sequences);
    compare(3U, stats_.replacements, shadow_stats.replacements);
    compare(4U, stats_.chunks, shadow_stats.chunks);
    compare(
        5U,
        stats_.maximum_pending_continuations,
        shadow_stats.maximum_pending_continuations);

    const std::uint64_t shadow_offset =
        zr_utf8_decoder_next_source_offset(&rust_shadow_storage_);
    if (next_source_offset_ != shadow_offset) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::NextSourceOffset,
            0U,
            next_source_offset_,
            shadow_offset);
        matches = false;
    }

    const std::uint64_t shadow_failed =
        zr_utf8_decoder_failed(&rust_shadow_storage_) != 0U ? 1U : 0U;
    const std::uint64_t primary_failed = failed_ ? 1U : 0U;
    if (primary_failed != shadow_failed) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::FailedState,
            0U,
            primary_failed,
            shadow_failed);
        matches = false;
    }

    const std::uint64_t shadow_policy =
        zr_utf8_decoder_policy(&rust_shadow_storage_);
    const std::uint64_t primary_policy =
        static_cast<std::uint64_t>(policy_);
    if (primary_policy != shadow_policy) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::Policy,
            0U,
            primary_policy,
            shadow_policy);
        matches = false;
    }
    return matches;
}

void Utf8StreamDecoder::rust_shadow_record_mismatch(
    RustShadowMismatchKind kind,
    std::uint64_t index,
    std::uint64_t expected,
    std::uint64_t actual) noexcept {
    increment_saturating(rust_shadow_mismatches_);
    if (rust_shadow_first_mismatch_ == RustShadowMismatchKind::None) {
        rust_shadow_first_mismatch_ = kind;
        rust_shadow_first_index_ = index;
        rust_shadow_expected_ = expected;
        rust_shadow_actual_ = actual;
    }
#if ZEVRYON_RUST_UNICODE_SHADOW_STRICT
    std::abort();
#endif
}

void Utf8StreamDecoder::increment_saturating(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}
#endif

} // namespace zevryon::text
