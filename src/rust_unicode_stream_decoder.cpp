#include "rust_unicode_stream_decoder.hpp"

#include <string>

namespace zevryon::text {

RustUtf8StreamDecoder::RustUtf8StreamDecoder(Utf8ErrorPolicy policy) noexcept
    : initialized_(
          zr_utf8_decoder_init(&storage_, policy_id(policy)) != 0U) {}

RustUtf8StreamDecoder::~RustUtf8StreamDecoder() {
    if (initialized_) {
        zr_utf8_decoder_clear(&storage_);
    }
    initialized_ = false;
}

bool RustUtf8StreamDecoder::valid() const noexcept {
    return initialized_ && zr_utf8_decoder_valid(&storage_) != 0U;
}

bool RustUtf8StreamDecoder::feed(
    std::span<const std::byte> bytes,
    std::uint64_t absolute_source_offset,
    std::span<ZrDecodedCodePoint> output,
    std::size_t* written,
    Utf8DecodeError* error) noexcept {
    if (!initialized_ || written == nullptr || error == nullptr) {
        return false;
    }

    ZrUtf8DecodeError ffi_error{};
    const auto* input = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const bool success =
        zr_utf8_decoder_feed(
            &storage_,
            input,
            bytes.size(),
            absolute_source_offset,
            output.data(),
            output.size(),
            written,
            &ffi_error) != 0U;
    copy_error(ffi_error, error);
    return success;
}

bool RustUtf8StreamDecoder::finish(
    std::span<ZrDecodedCodePoint> output,
    std::size_t* written,
    Utf8DecodeError* error) noexcept {
    if (!initialized_ || written == nullptr || error == nullptr) {
        return false;
    }

    ZrUtf8DecodeError ffi_error{};
    const bool success =
        zr_utf8_decoder_finish(
            &storage_,
            output.data(),
            output.size(),
            written,
            &ffi_error) != 0U;
    copy_error(ffi_error, error);
    return success;
}

bool RustUtf8StreamDecoder::reset() noexcept {
    return initialized_ && zr_utf8_decoder_reset(&storage_) != 0U;
}

Utf8ErrorPolicy RustUtf8StreamDecoder::policy() const noexcept {
    if (!initialized_) {
        return Utf8ErrorPolicy::Strict;
    }
    return policy_from_id(zr_utf8_decoder_policy(&storage_));
}

Utf8DecodeStats RustUtf8StreamDecoder::stats() const noexcept {
    if (!initialized_) {
        return {};
    }

    ZrUtf8DecodeStats ffi_stats{};
    if (zr_utf8_decoder_stats(&storage_, &ffi_stats) == 0U) {
        return {};
    }
    return {
        ffi_stats.source_bytes,
        ffi_stats.emitted_codepoints,
        ffi_stats.invalid_sequences,
        ffi_stats.replacements,
        ffi_stats.chunks,
        ffi_stats.maximum_pending_continuations,
    };
}

std::uint64_t RustUtf8StreamDecoder::next_source_offset() const noexcept {
    return initialized_ ? zr_utf8_decoder_next_source_offset(&storage_) : 0U;
}

bool RustUtf8StreamDecoder::failed() const noexcept {
    return !initialized_ || zr_utf8_decoder_failed(&storage_) != 0U;
}

std::uint32_t RustUtf8StreamDecoder::abi_version() noexcept {
    return zr_utf8_abi_version();
}

std::size_t RustUtf8StreamDecoder::storage_size() noexcept {
    return zr_utf8_decoder_storage_size();
}

std::size_t RustUtf8StreamDecoder::storage_alignment() noexcept {
    return zr_utf8_decoder_storage_alignment();
}

std::uint32_t RustUtf8StreamDecoder::policy_id(Utf8ErrorPolicy policy) noexcept {
    switch (policy) {
    case Utf8ErrorPolicy::Strict:
        return ZR_UTF8_POLICY_STRICT;
    case Utf8ErrorPolicy::Replace:
        return ZR_UTF8_POLICY_REPLACE;
    }
    return ZR_UTF8_POLICY_STRICT;
}

Utf8ErrorPolicy RustUtf8StreamDecoder::policy_from_id(std::uint32_t policy) noexcept {
    return policy == ZR_UTF8_POLICY_REPLACE
        ? Utf8ErrorPolicy::Replace
        : Utf8ErrorPolicy::Strict;
}

Utf8ErrorKind RustUtf8StreamDecoder::error_kind_from_id(std::uint32_t kind) noexcept {
    switch (kind) {
    case ZR_UTF8_ERROR_NONE:
        return Utf8ErrorKind::None;
    case ZR_UTF8_ERROR_DISCONTINUOUS_INPUT:
        return Utf8ErrorKind::DiscontinuousInput;
    case ZR_UTF8_ERROR_INVALID_LEAD_BYTE:
        return Utf8ErrorKind::InvalidLeadByte;
    case ZR_UTF8_ERROR_UNEXPECTED_CONTINUATION:
        return Utf8ErrorKind::UnexpectedContinuation;
    case ZR_UTF8_ERROR_INVALID_CONTINUATION:
        return Utf8ErrorKind::InvalidContinuation;
    case ZR_UTF8_ERROR_OVERLONG_ENCODING:
        return Utf8ErrorKind::OverlongEncoding;
    case ZR_UTF8_ERROR_SURROGATE_CODE_POINT:
        return Utf8ErrorKind::SurrogateCodePoint;
    case ZR_UTF8_ERROR_CODE_POINT_OUT_OF_RANGE:
        return Utf8ErrorKind::CodePointOutOfRange;
    case ZR_UTF8_ERROR_TRUNCATED_SEQUENCE:
        return Utf8ErrorKind::TruncatedSequence;
    case ZR_UTF8_ERROR_OUTPUT_BUDGET_EXCEEDED:
        return Utf8ErrorKind::OutputBudgetExceeded;
    default:
        return Utf8ErrorKind::DiscontinuousInput;
    }
}

const char* RustUtf8StreamDecoder::error_message(Utf8ErrorKind kind) noexcept {
    switch (kind) {
    case Utf8ErrorKind::None:
        return "";
    case Utf8ErrorKind::DiscontinuousInput:
        return "UTF-8 decoder lifecycle or input continuity failure";
    case Utf8ErrorKind::InvalidLeadByte:
        return "invalid UTF-8 lead byte";
    case Utf8ErrorKind::UnexpectedContinuation:
        return "unexpected UTF-8 continuation byte";
    case Utf8ErrorKind::InvalidContinuation:
        return "UTF-8 sequence contains a non-continuation byte";
    case Utf8ErrorKind::OverlongEncoding:
        return "overlong UTF-8 sequence";
    case Utf8ErrorKind::SurrogateCodePoint:
        return "UTF-8 sequence encodes a surrogate code point";
    case Utf8ErrorKind::CodePointOutOfRange:
        return "UTF-8 code point exceeds Unicode range";
    case Utf8ErrorKind::TruncatedSequence:
        return "UTF-8 input ended inside a sequence";
    case Utf8ErrorKind::OutputBudgetExceeded:
        return "UTF-8 output exceeded its resource budget";
    }
    return "invalid UTF-8 error";
}

void RustUtf8StreamDecoder::copy_error(
    const ZrUtf8DecodeError& source,
    Utf8DecodeError* destination) noexcept {
    if (destination == nullptr) {
        return;
    }
    destination->kind = error_kind_from_id(source.kind);
    destination->source_offset = source.source_offset;
    try {
        destination->message = error_message(destination->kind);
    } catch (...) {
        destination->message.clear();
    }
}

} // namespace zevryon::text
