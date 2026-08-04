# Z2R-3 — Rust Unicode stream foundation

## Objective

Z2R-3 creates a safe Rust implementation of Zevryon's streaming UTF-8 decoder and certifies it against the existing C++ `Utf8StreamDecoder` oracle. This slice establishes equivalence and a stable language boundary; it does not change the production Unicode authority.

## Fixed ABI

The Unicode boundary is caller-owned and allocation-neutral:

- decoder state: 128 bytes, aligned to 8 bytes;
- decoded code-point record: 16 bytes;
- statistics record: 48 bytes;
- error record: 16 bytes;
- input remains a caller-owned read-only byte span;
- output is written into a caller-provided fixed-capacity record array;
- no Rust `Vec`, C++ STL object, allocator, exception or panic crosses the ABI.

The ABI exposes strict and replacement policies plus the existing error taxonomy:

- discontinuous input;
- invalid lead byte;
- unexpected continuation;
- invalid continuation;
- overlong encoding;
- surrogate code point;
- code point outside the Unicode range;
- truncated sequence;
- output budget exceeded.

Invalid storage, null required pointers, misalignment, invalid policy values and exhausted output capacity fail closed.

## Safe core and unsafe boundary

`zevryon-unicode-stream` contains the complete UTF-8 state machine and forbids unsafe code. It owns no external allocation and emits records through a fallible callback.

`zevryon-unicode-ffi` is the only unsafe surface. Its audited operations are limited to:

- casting validated fixed storage to the decoder state;
- reading a caller-provided input byte range;
- writing bounded decoded records, statistics and error records;
- clearing the fixed storage record.

The decoder state carries an initialization magic value. Zeroed, cleared or otherwise uninitialized storage is rejected.

## Exact semantic contract

The Rust implementation mirrors the existing C++ behavior for:

- contiguous absolute source offsets across arbitrary chunk boundaries;
- ASCII and two-, three- and four-byte UTF-8 sequences;
- non-continuation retry behavior under replacement policy;
- strict failure latching;
- replacement ranges and source lengths;
- overlong, surrogate and out-of-range validation;
- truncated input handling during `finish()`;
- idempotent `finish()`;
- policy-preserving `reset()`;
- source byte saturation;
- wrapping event counters;
- maximum pending-continuation accounting;
- output-budget failure before a rejected record is counted.

## Certification

The standalone Unicode bridge builds only the Rust Unicode static library and two strict native targets:

1. C++ oracle equivalence;
2. pure C11 ABI runtime certification.

The C++ equivalence test compares after every feed and finish operation:

- return result;
- complete decoded record stream;
- error kind and source offset;
- all decoder statistics;
- next source offset;
- failed state;
- configured policy.

The corpus covers multilingual valid input at every chunk size, all malformed sequence classes, strict and replacement policies, truncation, discontinuity, source-range overflow, lifecycle transitions and explicit output-capacity exhaustion.

The focused workflow runs on Ubuntu, Windows and macOS and requires:

- `cargo fmt --check`;
- all workspace tests;
- Clippy with `-D warnings`;
- strict C and C++ builds;
- exact oracle equivalence;
- pure C ABI runtime success;
- the unchanged production C++ Unicode stream test with Rust disabled.

## Promotion boundary

This PR does not:

- replace `src/unicode_stream.cpp` in the production root graph;
- make Rust authoritative for Unicode decoding;
- add Rust to the default production build;
- change grapheme, script, bidi, font, GPU or platform ownership.

A later promotion slice may add production shadow execution and measured workload gates before any authority switch. The current rollback boundary is therefore trivial: the root production graph remains unchanged.
