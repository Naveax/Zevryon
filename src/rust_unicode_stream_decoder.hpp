#pragma once

#include "unicode_stream.hpp"
#include "zevryon_rust_ffi.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace zevryon::text {

class RustUtf8StreamDecoder final {
public:
    explicit RustUtf8StreamDecoder(
        Utf8ErrorPolicy policy = Utf8ErrorPolicy::Strict) noexcept;
    ~RustUtf8StreamDecoder();

    RustUtf8StreamDecoder(const RustUtf8StreamDecoder&) = delete;
    RustUtf8StreamDecoder& operator=(const RustUtf8StreamDecoder&) = delete;
    RustUtf8StreamDecoder(RustUtf8StreamDecoder&&) = delete;
    RustUtf8StreamDecoder& operator=(RustUtf8StreamDecoder&&) = delete;

    bool valid() const noexcept;
    bool feed(
        std::span<const std::byte> bytes,
        std::uint64_t absolute_source_offset,
        std::span<ZrDecodedCodePoint> output,
        std::size_t* written,
        Utf8DecodeError* error) noexcept;
    bool finish(
        std::span<ZrDecodedCodePoint> output,
        std::size_t* written,
        Utf8DecodeError* error) noexcept;
    bool reset() noexcept;

    Utf8ErrorPolicy policy() const noexcept;
    Utf8DecodeStats stats() const noexcept;
    std::uint64_t next_source_offset() const noexcept;
    bool failed() const noexcept;

    static std::uint32_t abi_version() noexcept;
    static std::size_t storage_size() noexcept;
    static std::size_t storage_alignment() noexcept;

private:
    static std::uint32_t policy_id(Utf8ErrorPolicy policy) noexcept;
    static Utf8ErrorPolicy policy_from_id(std::uint32_t policy) noexcept;
    static Utf8ErrorKind error_kind_from_id(std::uint32_t kind) noexcept;
    static const char* error_message(Utf8ErrorKind kind) noexcept;
    static void copy_error(
        const ZrUtf8DecodeError& source,
        Utf8DecodeError* destination) noexcept;

    ZrUtf8DecoderStorage storage_{};
    bool initialized_{false};
};

static_assert(sizeof(ZrDecodedCodePoint) == sizeof(DecodedCodePoint));
static_assert(alignof(ZrDecodedCodePoint) == alignof(DecodedCodePoint));
static_assert(sizeof(ZrUtf8DecodeStats) == 48U);
static_assert(sizeof(ZrUtf8DecodeError) == 16U);
static_assert(sizeof(ZrUtf8DecoderStorage) == ZR_UTF8_DECODER_STORAGE_BYTES);
static_assert(alignof(ZrUtf8DecoderStorage) == ZR_UTF8_DECODER_STORAGE_ALIGN);

} // namespace zevryon::text
