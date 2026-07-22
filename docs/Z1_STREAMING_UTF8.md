# Z1A — Bounded Streaming UTF-8

## Status

Z0 is implemented and evidence-backed. Z1 is active. This document covers the first Z1 slice only: streaming UTF-8 decoding and its memory contract.

Full Z1 still requires grapheme segmentation, script runs, bidi runs, Unicode line breaking, and a preserved ZENITH core regression gate.

## Contract

`Utf8StreamDecoder` consumes contiguous source chunks without requiring a complete record or source window in memory. Decoder state carries at most one UTF-8 sequence across chunk boundaries.

Every output record preserves:

- Unicode scalar value;
- absolute source byte start;
- absolute source byte end;
- whether the scalar is U+FFFD emitted by replacement policy.

The output record layout is ordered to remain at or below 24 bytes on supported 64-bit targets. A compile-time assertion prevents silent padding regression.

## Error policies

### Strict

The decoder fails on:

- invalid lead bytes;
- unexpected continuation bytes;
- invalid continuation bytes;
- overlong encodings;
- UTF-16 surrogate values;
- values above U+10FFFF;
- truncated sequences at end of stream;
- discontinuous source offsets;
- input after `finish()`;
- output budget exhaustion.

### Replace

Malformed sequences produce U+FFFD with the exact invalid byte range. If a pending sequence is followed by a non-continuation byte, the invalid sequence is replaced and the current byte is retried as a new lead. No source byte is silently dropped.

## Resource accounting

`LedgerMemoryResource` is a `std::pmr::memory_resource` adapter over the Z0 `ResourceLedger`.

Allocation order:

1. reserve the exact requested allocator bytes against a resource class;
2. reject before allocation when the hard limit would be exceeded;
3. call the upstream memory resource;
4. release the reservation if upstream allocation fails;
5. release the exact charged bytes during deallocation.

The UTF-8 benchmark charges its output vector to `ResourceClass::UnicodeBuffer`.

## Correctness tests

The test matrix includes:

- one-shot decode versus every possible chunk size;
- ASCII, Turkish letters, combining marks, CJK, Hebrew, Arabic, and emoji;
- absolute byte-range preservation;
- strict malformed-input rejection;
- deterministic replacement and byte retry;
- truncated sequences;
- reset, finish, and discontinuous offset behavior;
- hard-budget rejection;
- exact allocator release;
- Linux ASan/UBSan and dedicated MSVC ASan execution.

## Initial benchmark

Fixture:

- 64 KiB valid mixed UTF-8;
- 4 KiB source chunks;
- 32 warm-up iterations;
- 1,024 measured iterations;
- 4 MiB UnicodeBuffer hard limit.

The benchmark reports P50/P95/P99/maximum latency, decoded codepoint count, throughput, current and peak allocator bytes, rejected reservations, accounting errors, and hard-limit status.

The first pre-packing CI result was:

| Metric | Result |
|---|---:|
| P50 | 0.301 ms |
| P95 | 0.321 ms |
| P99 | 0.449 ms |
| Maximum | 0.482 ms |
| P50 throughput | 207.7 MiB/s |
| Decoded codepoints | 44,336 |
| Unicode allocator current | 2.0 MiB |
| Unicode allocator peak | 3.0 MiB |
| Hard limit | 4.0 MiB |
| Rejected reservations | 0 |
| Accounting errors | 0 |

The decoded record was subsequently packed from 32 bytes to at most 24 bytes. Final evidence must be taken from the post-packing workflow artifact rather than these historical baseline values.

## Boundary

This layer emits Unicode scalar values, not grapheme clusters or glyphs. It does not implement normalization, script segmentation, bidi reordering, font fallback, shaping, line breaking, layout, paint, selection, IME, or accessibility mapping.
