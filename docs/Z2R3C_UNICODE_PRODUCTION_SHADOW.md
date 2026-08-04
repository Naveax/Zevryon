# Z2R-3C — Production Rust Unicode shadow

## Objective

Z2R-3C connects the certified Z2R-3B Rust UTF-8 state machine to the real production `Utf8StreamDecoder` path as an optional, non-authoritative shadow.

The C++ decoder remains the production authority. Every valid production `feed`, `finish` and `reset` operation is mirrored into the Rust decoder. C++ return values, output records and state continue to control browser behavior.

## Build modes

### Default rollback-safe mode

```text
ZEVRYON_RUST_UNICODE_SHADOW=OFF
ZEVRYON_RUST_UNICODE_SHADOW_STRICT=OFF
```

The root graph remains C++ only. Cargo is not required for Unicode decoding, no Rust Unicode library is built or linked, and the decoder object contains no Rust storage or shadow buffer.

### Diagnostic production shadow

```text
ZEVRYON_RUST_UNICODE_SHADOW=ON
ZEVRYON_RUST_UNICODE_SHADOW_STRICT=OFF
```

The build selects the shared Rust toolchain, builds `zevryon-unicode-ffi`, embeds fixed Rust decoder storage in each production decoder and latches divergence telemetry without changing C++ results.

### Strict certification

```text
ZEVRYON_RUST_UNICODE_SHADOW=ON
ZEVRYON_RUST_UNICODE_SHADOW_STRICT=ON
```

The first mismatch terminates the certification process. This is the required CI mode and is not the default release configuration.

## Exact comparison contract

After each mirrored operation, production C++ and Rust are compared for:

- operation success or failure;
- appended output-record count;
- every appended code point value;
- source start and encoded byte length;
- replacement flag;
- error kind and source offset;
- source-byte count;
- emitted-codepoint count;
- invalid-sequence count;
- replacement count;
- chunk count;
- maximum pending continuations;
- next absolute source offset;
- failed-state latch;
- configured error policy.

Every operation performs a complete state verification. Diagnostics retain saturating operation, verification and mismatch counts plus the first mismatch category, record or field index, expected value and actual value.

Telemetry schema:

```text
zevryon.rust-unicode-shadow.v1
```

## Output-budget equivalence

The production C++ output vector can reject an allocation through `LedgerMemoryResource`. That is a legitimate externally imposed output-budget result, not a decoder divergence.

When C++ reports `OutputBudgetExceeded`, the Rust caller-owned output capacity is limited to the number of records C++ successfully appended before the failure. Rust therefore encounters the same bounded emission point and must return the same error without producing a false mismatch.

For normal operations, the reusable Rust shadow buffer is sized to the proven maximum output count for the supplied chunk. Shadow-buffer allocation failure is diagnostic and never changes the C++ result in non-strict mode.

## Safety and ownership

- Rust decoder state remains in fixed caller-owned storage.
- Input bytes remain caller-owned and read-only.
- Rust output records are written into a reusable C++ shadow buffer.
- No Rust collection, allocator or panic crosses the ABI.
- C++ PMR output ownership and resource accounting remain unchanged.
- Default builds preserve the pre-Z2R-3C object layout and dependency graph.

## Certification

The focused workflow runs on Ubuntu, Windows and macOS. Each runner requires:

- combined workspace formatting;
- all ledger, MassiveDoc and Unicode Rust tests;
- Clippy with `-D warnings`;
- an unchanged Rust-disabled C++ baseline build and Unicode test;
- a strict production Unicode-shadow build;
- real Unicode stream, grapheme, script-run and bidi-explicit production tests;
- zero strict-shadow mismatches, including PMR output-budget rejection.

Existing normal CI and Z2R workload workflows remain mandatory regression gates.

## Promotion boundary

This slice does not make Rust authoritative for UTF-8 decoding and does not remove `src/unicode_stream.cpp`.

Authority promotion requires a separate reviewed slice with:

- paired representative Unicode workload overhead evidence;
- exact Unicode 17 conformance parity under production shadow;
- adversarial divergence injection proving the C++ result remains authoritative;
- an explicit build-only rollback switch.
