# Z2R-3D-U — Rust Unicode shadow promotion readiness

## Purpose

This slice certifies the production Rust UTF-8 stream decoder shadow before any
authority switch. The C++ `Utf8StreamDecoder` remains authoritative.

The prerequisite is the exact Z2R-3C-U head:

```text
2a717a6a48ffbbd8c0b3d25f35c4320af41fdfed
```

## Production boundary

The positive certification pairs:

```text
baseline:
  ZEVRYON_RUST_UNICODE_SHADOW=OFF
  ZEVRYON_RUST_UNICODE_SHADOW_STRICT=OFF
  ZEVRYON_ENABLE_RUST_CORE=OFF

strict shadow:
  ZEVRYON_RUST_UNICODE_SHADOW=ON
  ZEVRYON_RUST_UNICODE_SHADOW_STRICT=ON
  ZEVRYON_RUST_UNICODE_SHADOW_TEST_HOOKS=OFF
```

The baseline build must remain Cargo-free. Rust does not supply browser-visible
return values, decoded records, errors, statistics, offsets, PMR ownership or
allocation decisions in this slice.

## Required corpus

Each platform executes three alternating baseline/shadow samples over a
deterministic 16 MiB valid UTF-8 corpus for two complete rounds. The chunk
schedule includes one-byte boundaries and sizes up to 64 KiB.

Every sample also covers:

- strict and replacement policies;
- invalid lead bytes;
- unexpected continuation bytes;
- invalid continuation bytes;
- overlong encodings;
- UTF-16 surrogate encodings;
- code points above U+10FFFF;
- truncated sequences;
- discontinuous source offsets;
- PMR output-budget exhaustion;
- decoder reset and reuse.

The canonical semantic hash excludes timing and Rust-shadow telemetry. It must
match across baseline/shadow samples and across Linux, Windows and macOS.

## Independent negative evidence

A Linux-only diagnostic build enables
`ZEVRYON_RUST_UNICODE_SHADOW_TEST_HOOKS`. Four separate processes set
`ZEVRYON_UTF8_SHADOW_FAULT` to:

```text
output
error
state
reset
```

The hooks corrupt only the private Rust-shadow comparison surface. C++
authoritative output and the semantic hash must remain unchanged. The expected
first mismatches are:

| Fault | Expected latch |
|---|---|
| output | `output_record` |
| error | `error_kind` |
| state | `statistics` |
| reset | `reset_result` |

No test hook is compiled into the strict production shadow or baseline builds.

## Performance and memory gates

The platform manifest enforces:

- P50 wall ratio no greater than `2.00x`;
- P95 wall ratio no greater than `2.25x`;
- P99 wall ratio no greater than `2.50x`;
- maximum wall ratio no greater than `3.00x`;
- peak RSS passes when either the ratio is no greater than `1.50x` or the
  absolute shadow delta is no greater than `16 MiB`.

These are hosted-runner certification limits, not end-user performance claims.

## Fail-closed evidence chain

The workflow emits:

1. one paired report per platform;
2. one SHA-256-bound platform certification per platform;
3. one final three-platform promotion-readiness manifest.

The finalizer rejects:

- mixed commits;
- forged report or manifest hashes;
- semantic divergence;
- missing fault classes;
- positive-run mismatches;
- topology drift;
- authority changes;
- loss of Cargo-free rollback;
- performance or memory gate failure.

## Promotion boundary

A successful Z2R-3D-U manifest means the Rust UTF-8 decoder is eligible for a
separate authority-switch review. It does not itself switch authority.

The following remain mandatory for the next slice:

- Rust-sourced public operation results and decoded records;
- C++ reverse-shadow execution;
- no silent fallback after Rust failure;
- adversarial proof that C++ verifier corruption cannot alter Rust public
  output;
- three-platform authority certification;
- immediate build-only rollback.
