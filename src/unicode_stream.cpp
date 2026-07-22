#include "unicode_stream.hpp"

#include <algorithm>
#include <limits>
#include <new>

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
    : policy_(policy) {}

bool Utf8StreamDecoder::feed(
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

bool Utf8StreamDecoder::finish(
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

} // namespace zevryon::text
