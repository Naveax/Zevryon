#include "unicode_stream.hpp"
#include "unicode_stream_cpp_reverse.hpp"

#if defined(ZEVRYON_UTF8_RUST_SHADOW)
#define ZEVRYON_RESTORE_UTF8_RUST_SHADOW 1
#undef ZEVRYON_UTF8_RUST_SHADOW
#endif
#if defined(ZEVRYON_UTF8_RUST_SHADOW_TEST_HOOKS)
#define ZEVRYON_RESTORE_UTF8_RUST_SHADOW_TEST_HOOKS 1
#undef ZEVRYON_UTF8_RUST_SHADOW_TEST_HOOKS
#endif
#define zevryon zevryon_cpp_reverse
#include "unicode_stream.cpp"
#undef zevryon
#if defined(ZEVRYON_RESTORE_UTF8_RUST_SHADOW_TEST_HOOKS)
#define ZEVRYON_UTF8_RUST_SHADOW_TEST_HOOKS 1
#undef ZEVRYON_RESTORE_UTF8_RUST_SHADOW_TEST_HOOKS
#endif
#if defined(ZEVRYON_RESTORE_UTF8_RUST_SHADOW)
#define ZEVRYON_UTF8_RUST_SHADOW 1
#undef ZEVRYON_RESTORE_UTF8_RUST_SHADOW
#endif

#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <sstream>

namespace zevryon::text {
namespace {

using ReverseDecoder = zevryon_cpp_reverse::text::Utf8StreamDecoder;
using ReverseError = zevryon_cpp_reverse::text::Utf8DecodeError;
using ReverseErrorKind = zevryon_cpp_reverse::text::Utf8ErrorKind;
using ReversePoint = zevryon_cpp_reverse::text::DecodedCodePoint;
using ReversePolicy = zevryon_cpp_reverse::text::Utf8ErrorPolicy;

ReversePolicy reverse_policy(Utf8ErrorPolicy policy) noexcept {
    return policy == Utf8ErrorPolicy::Strict
               ? ReversePolicy::Strict
               : ReversePolicy::Replace;
}

void clear_public_error(Utf8DecodeError* error) noexcept {
    if (error == nullptr) {
        return;
    }
    error->kind = Utf8ErrorKind::None;
    error->source_offset = 0U;
    error->message.clear();
}

std::uint64_t message_fingerprint(const std::string& message) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char character : message) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    return hash;
}

Utf8ErrorKind error_kind_from_abi(std::uint32_t kind) noexcept {
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
            std::abort();
    }
}

const char* authority_error_message(
    Utf8ErrorKind kind,
    std::uint32_t detail) noexcept {
    switch (kind) {
        case Utf8ErrorKind::None:
            return detail == ZR_UTF8_ERROR_DETAIL_NONE ? "" : nullptr;
        case Utf8ErrorKind::DiscontinuousInput:
            switch (detail) {
                case ZR_UTF8_ERROR_DETAIL_DECODER_FAILED:
                    return "UTF-8 decoder is in a failed state";
                case ZR_UTF8_ERROR_DETAIL_DECODER_FINISHED:
                    return "UTF-8 decoder already finished";
                case ZR_UTF8_ERROR_DETAIL_DISCONTINUOUS_OFFSET:
                    return "UTF-8 input chunks are not contiguous";
                case ZR_UTF8_ERROR_DETAIL_SOURCE_RANGE_OVERFLOW:
                    return "UTF-8 source range overflows 64-bit offsets";
                default:
                    return nullptr;
            }
        case Utf8ErrorKind::InvalidLeadByte:
            return detail == ZR_UTF8_ERROR_DETAIL_NONE
                       ? "invalid UTF-8 lead byte"
                       : nullptr;
        case Utf8ErrorKind::UnexpectedContinuation:
            return detail == ZR_UTF8_ERROR_DETAIL_NONE
                       ? "unexpected UTF-8 continuation byte"
                       : nullptr;
        case Utf8ErrorKind::InvalidContinuation:
            return detail == ZR_UTF8_ERROR_DETAIL_NONE
                       ? "UTF-8 sequence contains a non-continuation byte"
                       : nullptr;
        case Utf8ErrorKind::OverlongEncoding:
            return detail == ZR_UTF8_ERROR_DETAIL_NONE
                       ? "overlong UTF-8 sequence"
                       : nullptr;
        case Utf8ErrorKind::SurrogateCodePoint:
            return detail == ZR_UTF8_ERROR_DETAIL_NONE
                       ? "UTF-8 sequence encodes a surrogate code point"
                       : nullptr;
        case Utf8ErrorKind::CodePointOutOfRange:
            return detail == ZR_UTF8_ERROR_DETAIL_NONE
                       ? "UTF-8 code point exceeds Unicode range"
                       : nullptr;
        case Utf8ErrorKind::TruncatedSequence:
            return detail == ZR_UTF8_ERROR_DETAIL_NONE
                       ? "UTF-8 input ended inside a sequence"
                       : nullptr;
        case Utf8ErrorKind::OutputBudgetExceeded:
            return detail == ZR_UTF8_ERROR_DETAIL_OUTPUT_CAPACITY
                       ? "UTF-8 output exceeded its resource budget"
                       : nullptr;
    }
    return nullptr;
}

void write_public_error(
    Utf8DecodeError* error,
    const ZrUtf8DecodeError& rust_error) noexcept {
    if (error == nullptr) {
        return;
    }
    error->kind = error_kind_from_abi(rust_error.kind);
    error->source_offset = rust_error.source_offset;
    const char* message =
        authority_error_message(error->kind, rust_error.detail);
    if (message == nullptr) {
        std::abort();
    }
    try {
        error->message = message;
    } catch (...) {
        error->message.clear();
    }
}

DecodedCodePoint public_point(const ZrDecodedCodePoint& point) noexcept {
    return DecodedCodePoint{
        point.value,
        point.source_start,
        point.source_start + static_cast<std::uint64_t>(point.source_length),
        point.replacement != 0U};
}

#if defined(ZEVRYON_UTF8_RUST_AUTHORITY_TEST_HOOKS)
bool cpp_authority_fault(const char* expected) noexcept {
    const char* selected = std::getenv("ZEVRYON_UTF8_AUTHORITY_CPP_FAULT");
    return selected != nullptr && std::strcmp(selected, expected) == 0;
}
#endif

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
    try {
        cpp_reverse_ = std::make_unique<ReverseDecoder>(reverse_policy(policy));
    } catch (...) {
        std::abort();
    }

    rust_shadow_initialized_ =
        zr_utf8_decoder_init(
            &rust_shadow_storage_,
            static_cast<std::uint32_t>(policy_)) != 0U;
    if (!rust_shadow_initialized_) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::RustUnavailable, 0U, 1U, 0U);
        std::abort();
    }
    if (zr_utf8_abi_version() != ZR_UTF8_ABI_VERSION) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::AbiVersion,
            0U,
            ZR_UTF8_ABI_VERSION,
            zr_utf8_abi_version());
        std::abort();
    }
    if (zr_utf8_decoder_storage_size() != ZR_UTF8_DECODER_STORAGE_BYTES) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::StorageContract,
            0U,
            ZR_UTF8_DECODER_STORAGE_BYTES,
            zr_utf8_decoder_storage_size());
        std::abort();
    }
    if (zr_utf8_decoder_storage_alignment() != ZR_UTF8_DECODER_STORAGE_ALIGN) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::StorageContract,
            1U,
            ZR_UTF8_DECODER_STORAGE_ALIGN,
            zr_utf8_decoder_storage_alignment());
        std::abort();
    }
    if (!rust_shadow_verify_state()) {
        std::abort();
    }
}

Utf8StreamDecoder::~Utf8StreamDecoder() {
    if (rust_shadow_initialized_) {
        zr_utf8_decoder_clear(&rust_shadow_storage_);
    }
    rust_shadow_initialized_ = false;
    cpp_reverse_.reset();
}

bool Utf8StreamDecoder::feed(
    std::span<const std::byte> bytes,
    std::uint64_t absolute_source_offset,
    std::pmr::vector<DecodedCodePoint>* output,
    Utf8DecodeError* error) noexcept {
    if (output == nullptr || error == nullptr) {
        return false;
    }
    increment_saturating(rust_shadow_operations_);
    clear_public_error(error);

    const std::size_t output_start = output->size();
    bool public_budget_available = true;
    std::size_t capacity = 0U;
    if (bytes.size() == std::numeric_limits<std::size_t>::max() ||
        output_start > std::numeric_limits<std::size_t>::max() - bytes.size() - 1U) {
        public_budget_available = false;
    } else {
        capacity = bytes.size() + 1U;
        try {
            output->reserve(output_start + capacity);
        } catch (...) {
            public_budget_available = false;
        }
    }

    try {
        rust_shadow_output_.resize(public_budget_available ? capacity : 0U);
    } catch (...) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::BufferAllocation,
            0U,
            static_cast<std::uint64_t>(capacity),
            0U);
        std::abort();
    }

    std::size_t written = 0U;
    ZrUtf8DecodeError rust_error{};
    const auto* input = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const bool rust_result =
        zr_utf8_decoder_feed(
            &rust_shadow_storage_,
            input,
            bytes.size(),
            absolute_source_offset,
            rust_shadow_output_.data(),
            rust_shadow_output_.size(),
            &written,
            &rust_error) != 0U;

    if (written > rust_shadow_output_.size()) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::RustUnavailable,
            2U,
            static_cast<std::uint64_t>(rust_shadow_output_.size()),
            static_cast<std::uint64_t>(written));
        std::abort();
    }
    if (!rust_result && rust_error.kind == ZR_UTF8_ERROR_NONE) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::RustUnavailable, 3U, 1U, 0U);
        std::abort();
    }

    try {
        for (std::size_t index = 0U; index < written; ++index) {
            output->push_back(public_point(rust_shadow_output_[index]));
        }
    } catch (...) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::BufferAllocation,
            1U,
            static_cast<std::uint64_t>(written),
            static_cast<std::uint64_t>(output->size() - output_start));
        std::abort();
    }
    write_public_error(error, rust_error);
    if (!rust_shadow_verify_state()) {
        std::abort();
    }

    std::pmr::memory_resource* reverse_resource =
        public_budget_available
            ? std::pmr::new_delete_resource()
            : std::pmr::null_memory_resource();
    std::pmr::vector<ReversePoint> reverse_output(reverse_resource);
    ReverseError reverse_error{};
    const bool reverse_result = cpp_reverse_->feed(
        bytes,
        absolute_source_offset,
        &reverse_output,
        &reverse_error);

    bool compared_reverse_result = reverse_result;
    std::uint64_t compared_reverse_error_kind =
        static_cast<std::uint64_t>(reverse_error.kind);
    std::uint64_t compared_reverse_error_offset = reverse_error.source_offset;
#if defined(ZEVRYON_UTF8_RUST_AUTHORITY_TEST_HOOKS)
    if (cpp_authority_fault("error")) {
        compared_reverse_error_kind ^= 1U;
    }
#endif

    if (compared_reverse_result != rust_result) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::OperationResult,
            0U,
            rust_result ? 1U : 0U,
            compared_reverse_result ? 1U : 0U);
    }
    if (reverse_output.size() != written) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::OutputCount,
            0U,
            static_cast<std::uint64_t>(written),
            static_cast<std::uint64_t>(reverse_output.size()));
    }

    const std::size_t comparable = std::min(written, reverse_output.size());
    for (std::size_t index = 0U; index < comparable; ++index) {
        const ZrDecodedCodePoint& rust = rust_shadow_output_[index];
        const ReversePoint& reverse = reverse_output[index];
        std::uint32_t reverse_value = reverse.value;
#if defined(ZEVRYON_UTF8_RUST_AUTHORITY_TEST_HOOKS)
        if (index == 0U && cpp_authority_fault("output")) {
            reverse_value ^= 1U;
        }
#endif
        const std::uint64_t record_index = static_cast<std::uint64_t>(index) * 4U;
        if (rust.source_start != reverse.source_start) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::OutputRecord,
                record_index,
                rust.source_start,
                reverse.source_start);
        }
        if (rust.value != reverse_value) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::OutputRecord,
                record_index + 1U,
                rust.value,
                reverse_value);
        }
        if (rust.source_length != reverse.source_length) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::OutputRecord,
                record_index + 2U,
                rust.source_length,
                reverse.source_length);
        }
        const std::uint64_t reverse_replacement = reverse.replacement ? 1U : 0U;
        if (rust.replacement != reverse_replacement) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::OutputRecord,
                record_index + 3U,
                rust.replacement,
                reverse_replacement);
        }
    }

    if (rust_error.kind != compared_reverse_error_kind) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::ErrorKind,
            0U,
            rust_error.kind,
            compared_reverse_error_kind);
    }
    if (rust_error.source_offset != compared_reverse_error_offset) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::ErrorOffset,
            0U,
            rust_error.source_offset,
            compared_reverse_error_offset);
    }
    if (error->message != reverse_error.message) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::ErrorMessage,
            0U,
            message_fingerprint(error->message),
            message_fingerprint(reverse_error.message));
    }

    const auto& reverse_stats = cpp_reverse_->stats();
    std::uint64_t reverse_source_bytes = reverse_stats.source_bytes;
#if defined(ZEVRYON_UTF8_RUST_AUTHORITY_TEST_HOOKS)
    if (cpp_authority_fault("state")) {
        reverse_source_bytes ^= 1U;
    }
#endif
    const std::uint64_t expected_stats[] = {
        stats_.source_bytes,
        stats_.emitted_codepoints,
        stats_.invalid_sequences,
        stats_.replacements,
        stats_.chunks,
        stats_.maximum_pending_continuations};
    const std::uint64_t reverse_values[] = {
        reverse_source_bytes,
        reverse_stats.emitted_codepoints,
        reverse_stats.invalid_sequences,
        reverse_stats.replacements,
        reverse_stats.chunks,
        reverse_stats.maximum_pending_continuations};
    for (std::uint64_t index = 0U; index < 6U; ++index) {
        if (expected_stats[index] != reverse_values[index]) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::Statistics,
                index,
                expected_stats[index],
                reverse_values[index]);
        }
    }
    if (next_source_offset_ != cpp_reverse_->next_source_offset()) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::NextSourceOffset,
            0U,
            next_source_offset_,
            cpp_reverse_->next_source_offset());
    }
    const std::uint64_t reverse_failed = cpp_reverse_->failed() ? 1U : 0U;
    const std::uint64_t rust_failed = failed_ ? 1U : 0U;
    if (rust_failed != reverse_failed) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::FailedState,
            0U,
            rust_failed,
            reverse_failed);
    }
    return rust_result;
}

bool Utf8StreamDecoder::finish(
    std::pmr::vector<DecodedCodePoint>* output,
    Utf8DecodeError* error) noexcept {
    if (output == nullptr || error == nullptr) {
        return false;
    }
    increment_saturating(rust_shadow_operations_);
    clear_public_error(error);

    const std::size_t output_start = output->size();
    bool public_budget_available = true;
    try {
        if (output_start == std::numeric_limits<std::size_t>::max()) {
            public_budget_available = false;
        } else {
            output->reserve(output_start + 1U);
        }
    } catch (...) {
        public_budget_available = false;
    }

    try {
        rust_shadow_output_.resize(public_budget_available ? 1U : 0U);
    } catch (...) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::BufferAllocation, 2U, 1U, 0U);
        std::abort();
    }

    std::size_t written = 0U;
    ZrUtf8DecodeError rust_error{};
    const bool rust_result =
        zr_utf8_decoder_finish(
            &rust_shadow_storage_,
            rust_shadow_output_.data(),
            rust_shadow_output_.size(),
            &written,
            &rust_error) != 0U;
    if (written > rust_shadow_output_.size()) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::RustUnavailable,
            4U,
            static_cast<std::uint64_t>(rust_shadow_output_.size()),
            static_cast<std::uint64_t>(written));
        std::abort();
    }
    if (!rust_result && rust_error.kind == ZR_UTF8_ERROR_NONE) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::RustUnavailable, 5U, 1U, 0U);
        std::abort();
    }
    try {
        for (std::size_t index = 0U; index < written; ++index) {
            output->push_back(public_point(rust_shadow_output_[index]));
        }
    } catch (...) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::BufferAllocation,
            3U,
            static_cast<std::uint64_t>(written),
            static_cast<std::uint64_t>(output->size() - output_start));
        std::abort();
    }
    write_public_error(error, rust_error);
    if (!rust_shadow_verify_state()) {
        std::abort();
    }

    std::pmr::memory_resource* reverse_resource =
        public_budget_available
            ? std::pmr::new_delete_resource()
            : std::pmr::null_memory_resource();
    std::pmr::vector<ReversePoint> reverse_output(reverse_resource);
    ReverseError reverse_error{};
    const bool reverse_result = cpp_reverse_->finish(&reverse_output, &reverse_error);

    if (reverse_result != rust_result) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::OperationResult,
            1U,
            rust_result ? 1U : 0U,
            reverse_result ? 1U : 0U);
    }
    if (reverse_output.size() != written) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::OutputCount,
            1U,
            static_cast<std::uint64_t>(written),
            static_cast<std::uint64_t>(reverse_output.size()));
    }
    if (written != 0U && !reverse_output.empty()) {
        const ZrDecodedCodePoint& rust = rust_shadow_output_[0];
        const ReversePoint& reverse = reverse_output[0];
        std::uint32_t reverse_value = reverse.value;
#if defined(ZEVRYON_UTF8_RUST_AUTHORITY_TEST_HOOKS)
        if (cpp_authority_fault("output")) {
            reverse_value ^= 1U;
        }
#endif
        if (rust.source_start != reverse.source_start) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::OutputRecord,
                0U,
                rust.source_start,
                reverse.source_start);
        }
        if (rust.value != reverse_value) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::OutputRecord,
                1U,
                rust.value,
                reverse_value);
        }
        if (rust.source_length != reverse.source_length) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::OutputRecord,
                2U,
                rust.source_length,
                reverse.source_length);
        }
        const std::uint64_t reverse_replacement = reverse.replacement ? 1U : 0U;
        if (rust.replacement != reverse_replacement) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::OutputRecord,
                3U,
                rust.replacement,
                reverse_replacement);
        }
    }

    std::uint64_t reverse_error_kind =
        static_cast<std::uint64_t>(reverse_error.kind);
#if defined(ZEVRYON_UTF8_RUST_AUTHORITY_TEST_HOOKS)
    if (cpp_authority_fault("error")) {
        reverse_error_kind ^= 1U;
    }
#endif
    if (rust_error.kind != reverse_error_kind) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::ErrorKind,
            1U,
            rust_error.kind,
            reverse_error_kind);
    }
    if (rust_error.source_offset != reverse_error.source_offset) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::ErrorOffset,
            1U,
            rust_error.source_offset,
            reverse_error.source_offset);
    }
    if (error->message != reverse_error.message) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::ErrorMessage,
            1U,
            message_fingerprint(error->message),
            message_fingerprint(reverse_error.message));
    }
    return rust_result;
}

void Utf8StreamDecoder::reset() noexcept {
    increment_saturating(rust_shadow_operations_);
    rust_shadow_output_.clear();
    const bool rust_result =
        zr_utf8_decoder_reset(&rust_shadow_storage_) != 0U;
    cpp_reverse_->reset();
    bool reverse_result = true;
#if defined(ZEVRYON_UTF8_RUST_AUTHORITY_TEST_HOOKS)
    if (cpp_authority_fault("reset")) {
        reverse_result = false;
    }
#endif
    if (rust_result != reverse_result) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::ResetResult,
            0U,
            rust_result ? 1U : 0U,
            reverse_result ? 1U : 0U);
    }
    if (!rust_result || !rust_shadow_verify_state()) {
        std::abort();
    }
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
    return rust_shadow_initialized_;
}

bool Utf8StreamDecoder::rust_shadow_healthy() const noexcept {
    return rust_shadow_initialized_ && rust_shadow_mismatches_ == 0U;
}

std::uint64_t Utf8StreamDecoder::rust_shadow_operations() const noexcept {
    return rust_shadow_operations_;
}

std::uint64_t Utf8StreamDecoder::rust_shadow_verifications() const noexcept {
    return rust_shadow_verifications_;
}

std::uint64_t Utf8StreamDecoder::rust_shadow_mismatches() const noexcept {
    return rust_shadow_mismatches_;
}

std::string Utf8StreamDecoder::rust_shadow_json() const {
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
        case RustShadowMismatchKind::ErrorMessage:
            mismatch_name = "error_message";
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
    output << "{\"schema\":\"zevryon.rust-unicode-authority.v1\","
           << "\"enabled\":"
           << (rust_shadow_initialized_ ? "true" : "false") << ','
           << "\"authoritative_backend\":\"rust\","
           << "\"reverse_shadow_backend\":\"cpp\","
           << "\"fallback_permitted\":false,"
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
}

bool Utf8StreamDecoder::rust_shadow_verify_state() noexcept {
    increment_saturating(rust_shadow_verifications_);
    if (!rust_shadow_initialized_) {
        return false;
    }
    ZrUtf8DecodeStats rust_stats{};
    if (zr_utf8_decoder_stats(&rust_shadow_storage_, &rust_stats) == 0U) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::RustUnavailable, 6U, 1U, 0U);
        return false;
    }
    stats_.source_bytes = rust_stats.source_bytes;
    stats_.emitted_codepoints = rust_stats.emitted_codepoints;
    stats_.invalid_sequences = rust_stats.invalid_sequences;
    stats_.replacements = rust_stats.replacements;
    stats_.chunks = rust_stats.chunks;
    stats_.maximum_pending_continuations =
        rust_stats.maximum_pending_continuations;
    next_source_offset_ =
        zr_utf8_decoder_next_source_offset(&rust_shadow_storage_);
    failed_ = zr_utf8_decoder_failed(&rust_shadow_storage_) != 0U;
    const std::uint64_t rust_policy =
        zr_utf8_decoder_policy(&rust_shadow_storage_);
    const std::uint64_t expected_policy =
        static_cast<std::uint64_t>(policy_);
    if (rust_policy != expected_policy) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::Policy,
            0U,
            expected_policy,
            rust_policy);
        return false;
    }
    return true;
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

} // namespace zevryon::text
