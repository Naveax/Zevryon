#include "zevryon_rust_ffi.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(ZrDecodedCodePoint) == 16u, "UTF-8 output ABI size");
_Static_assert(sizeof(ZrUtf8DecodeStats) == 48u, "UTF-8 stats ABI size");
_Static_assert(sizeof(ZrUtf8DecodeError) == 16u, "UTF-8 error ABI size");
_Static_assert(
    sizeof(ZrUtf8DecoderStorage) == ZR_UTF8_DECODER_STORAGE_BYTES,
    "UTF-8 storage ABI size");
_Static_assert(
    _Alignof(ZrUtf8DecoderStorage) == ZR_UTF8_DECODER_STORAGE_ALIGN,
    "UTF-8 storage ABI alignment");

static int require(int condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", message);
        return 0;
    }
    return 1;
}

int main(void) {
    ZrUtf8DecoderStorage storage;
    ZrDecodedCodePoint output[4];
    ZrUtf8DecodeStats stats;
    ZrUtf8DecodeError error;
    size_t written = 0u;
    const uint8_t valid[] = {0x41u, 0xc5u, 0x9fu};
    const uint8_t malformed[] = {0xe2u, 0x28u, 0xa1u};

    memset(&storage, 0, sizeof(storage));
    memset(output, 0, sizeof(output));
    memset(&stats, 0, sizeof(stats));
    memset(&error, 0, sizeof(error));

    if (!require(zr_utf8_abi_version() == ZR_UTF8_ABI_VERSION, "ABI version") ||
        !require(
            zr_utf8_decoder_storage_size() == ZR_UTF8_DECODER_STORAGE_BYTES,
            "storage size export") ||
        !require(
            zr_utf8_decoder_storage_alignment() == ZR_UTF8_DECODER_STORAGE_ALIGN,
            "storage alignment export") ||
        !require(zr_utf8_decoder_valid(&storage) == 0u, "zero storage is invalid") ||
        !require(
            zr_utf8_decoder_init(&storage, ZR_UTF8_POLICY_STRICT) == 1u,
            "strict initialization") ||
        !require(zr_utf8_decoder_valid(&storage) == 1u, "initialized storage is valid") ||
        !require(
            zr_utf8_decoder_policy(&storage) == ZR_UTF8_POLICY_STRICT,
            "strict policy export")) {
        return 1;
    }

    if (!require(
            zr_utf8_decoder_feed(
                &storage,
                valid,
                sizeof(valid),
                100u,
                output,
                4u,
                &written,
                &error) == 1u,
            "valid feed") ||
        !require(written == 2u, "valid output count") ||
        !require(output[0].value == 0x41u, "ASCII output") ||
        !require(output[0].source_start == 100u, "ASCII source start") ||
        !require(output[0].source_length == 1u, "ASCII source length") ||
        !require(output[1].value == 0x15fu, "multibyte output") ||
        !require(output[1].source_start == 101u, "multibyte source start") ||
        !require(output[1].source_length == 2u, "multibyte source length") ||
        !require(error.kind == ZR_UTF8_ERROR_NONE, "valid feed clears error") ||
        !require(
            zr_utf8_decoder_finish(
                &storage,
                output,
                4u,
                &written,
                &error) == 1u,
            "finish") ||
        !require(written == 0u, "finish emits no extra records") ||
        !require(zr_utf8_decoder_stats(&storage, &stats) == 1u, "stats export") ||
        !require(stats.source_bytes == 3u, "source byte count") ||
        !require(stats.emitted_codepoints == 2u, "codepoint count") ||
        !require(stats.chunks == 1u, "chunk count") ||
        !require(stats.maximum_pending_continuations == 1u, "pending maximum") ||
        !require(
            zr_utf8_decoder_next_source_offset(&storage) == 103u,
            "next source offset") ||
        !require(zr_utf8_decoder_failed(&storage) == 0u, "valid decoder healthy")) {
        return 1;
    }

    if (!require(zr_utf8_decoder_reset(&storage) == 1u, "reset") ||
        !require(
            zr_utf8_decoder_feed(
                &storage,
                malformed,
                sizeof(malformed),
                0u,
                output,
                4u,
                &written,
                &error) == 0u,
            "strict malformed feed rejected") ||
        !require(
            error.kind == ZR_UTF8_ERROR_INVALID_CONTINUATION,
            "strict malformed error kind") ||
        !require(error.source_offset == 1u, "strict malformed error offset") ||
        !require(zr_utf8_decoder_failed(&storage) == 1u, "strict failure latched")) {
        return 1;
    }

    zr_utf8_decoder_clear(&storage);
    if (!require(zr_utf8_decoder_valid(&storage) == 0u, "clear invalidates storage") ||
        !require(
            zr_utf8_decoder_init(&storage, ZR_UTF8_POLICY_REPLACE) == 1u,
            "replace initialization") ||
        !require(
            zr_utf8_decoder_feed(
                &storage,
                malformed,
                sizeof(malformed),
                200u,
                output,
                4u,
                &written,
                &error) == 1u,
            "replace malformed feed") ||
        !require(written == 3u, "replace output count") ||
        !require(output[0].value == 0xfffdu, "replacement character") ||
        !require(output[0].replacement == 1u, "replacement flag") ||
        !require(output[1].value == 0x28u, "invalid continuation retried") ||
        !require(output[2].value == 0xfffdu, "unexpected continuation replaced") ||
        !require(zr_utf8_decoder_reset(&storage) == 1u, "replace reset") ||
        !require(
            zr_utf8_decoder_feed(
                &storage,
                valid,
                sizeof(valid),
                0u,
                NULL,
                0u,
                &written,
                &error) == 0u,
            "zero output capacity fails closed") ||
        !require(
            error.kind == ZR_UTF8_ERROR_OUTPUT_BUDGET_EXCEEDED,
            "output budget error kind") ||
        !require(written == 0u, "output budget writes no records")) {
        return 1;
    }

    zr_utf8_decoder_clear(&storage);
    puts("Rust UTF-8 C ABI tests passed");
    return 0;
}
