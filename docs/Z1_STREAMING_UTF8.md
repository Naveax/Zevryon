# Z1A — Bounded Streaming UTF-8

## Status

Z0 is implemented and evidence-backed. Z1 is active. This document covers the first Z1 slice only: streaming UTF-8 decoding and its memory contract.

Full Z1 still requires grapheme segmentation, script runs, bidi runs, Unicode line breaking, and a preserved ZENITH core regression gate.

## Contract

`Utf8StreamDecoder` consumes contiguous source chunks without requiring a complete record or source window in memory. Decoder state carries at most one UTF-8 sequence across chunk boundaries.

Every output record preserves:

- Unicode scalar value;
- absolute source byte start;
- source byte length;
- derived absolute source byte end;
- whether the scalar is U+FFFD emitted by replacement policy.

UTF-8 scalar source length is always between one and four bytes, so a second 64-bit end offset is unnecessary. `DecodedCodePoint` stores one 64-bit start, one 32-bit scalar, one 8-bit length, and one replacement flag. A compile-time assertion keeps the complete record at or below **16 bytes** on supported targets.

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
- source lengths restricted to one through four bytes;
- strict malformed-input rejection;
- deterministic replacement and byte retry;
- truncated sequences;
- reset, finish, and discontinuous offset behavior;
- hard-budget rejection;
- exact allocator release;
- Linux ASan/UBSan and dedicated MSVC ASan execution.

## Benchmark contract

Fixture:

- 64 KiB valid mixed UTF-8;
- 4 KiB source chunks;
- 32 warm-up iterations;
- 1,024 measured iterations;
- 2 MiB UnicodeBuffer hard limit in CI.

Hard gates:

| Gate | Limit |
|---|---:|
| Decode P95 | ≤0.50 ms |
| Decode P99 | ≤0.75 ms |
| UnicodeBuffer peak | ≤2 MiB |
| Rejected reservations | 0 |
| Accounting errors | 0 |

The benchmark reports P50/P95/P99/maximum latency, decoded codepoint count, throughput, current and peak allocator bytes, rejected reservations, accounting errors, and hard-limit status.

Historical packing progression on the same benchmark shape:

| Record layout | Current bytes | Peak bytes | P95 |
|---|---:|---:|---:|
| 32-byte record | 2.0 MiB | 3.0 MiB | 0.321 ms |
| 24-byte record | 1.5 MiB | 2.25 MiB | 0.305 ms |
| 16-byte record | Final artifact required | Final artifact required | Final artifact required |

Only the final post-16-byte workflow artifact is certification evidence.

## Boundary

This layer emits Unicode scalar values, not grapheme clusters or glyphs. It does not implement normalization, script segmentation, bidi reordering, font fallback, shaping, line breaking, layout, paint, selection, IME, or accessibility mapping.
