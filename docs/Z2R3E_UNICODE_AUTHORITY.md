# Z2R-3E-U — Rust UTF-8 Decoder Authority

## Scope

Z2R-3E-U promotes the already certified Rust UTF-8 stream decoder from production shadow execution to an explicit, opt-in authoritative backend.

The prerequisite promotion-readiness slice is Z2R-3D-U:

```text
certified head: 30d2a48ed6857a0a43cd4200af631f1565e54729
promotion manifest: b11298f67aae593d1783a3870589d28fe5809ea3ce539150a85383de9f0e6f22
semantic SHA-256: 90d18dc834c8994bad4c136148d2c976e3a61237414479603f1866af86c9d753
```

Current desktop certification follows the repository support contract on `main`: Windows and Linux are supported; macOS is intentionally unsupported. Z2R-3E-U therefore requires supported-platform authority evidence from Windows and Linux only. A macOS-labeled validation result is not admissible authority evidence.

## Authority boundary

When authority mode is enabled:

- Rust supplies the public `feed` and `finish` operation result;
- Rust supplies every public decoded code-point record;
- Rust supplies the public error kind, detail code and source offset;
- Rust supplies statistics, failed state and next source offset;
- C++ executes the same operation only as a private reverse shadow;
- no C++ result is used as fallback after Rust failure;
- ABI, storage-contract or impossible FFI failure terminates fail-closed.

The human-readable error message is generated deterministically from the Rust error kind and detail code. It is not copied from the C++ verifier. The generated message is then compared byte-for-byte with the private C++ reverse-shadow message.

## UTF-8 ABI 1.1 error details

The error record remains 16 bytes:

```text
uint32 kind
uint32 detail
uint64 source_offset
```

The former reserved field is now a versioned machine-readable detail. The UTF-8 ABI version is `0x00010001`; record size and alignment do not change.

| Detail | Value | Public message |
|---|---:|---|
| `none` | 0 | selected solely by the error kind |
| `decoder_failed` | 1 | `UTF-8 decoder is in a failed state` |
| `decoder_finished` | 2 | `UTF-8 decoder already finished` |
| `discontinuous_offset` | 3 | `UTF-8 input chunks are not contiguous` |
| `source_range_overflow` | 4 | `UTF-8 source range overflows 64-bit offsets` |
| `output_capacity` | 5 | `UTF-8 output exceeded its resource budget` |

Unknown kind/detail combinations are not generalized and are not eligible for fallback. Authority execution terminates fail-closed.

## Build modes

### Default C++ production and immediate rollback

```text
ZEVRYON_RUST_UNICODE_AUTHORITATIVE=OFF
ZEVRYON_RUST_UNICODE_SHADOW=OFF
ZEVRYON_ENABLE_RUST_CORE=OFF
```

The original `src/unicode_stream.cpp` remains the production implementation. Cargo and the Rust Unicode static library are absent from the UTF-8 production graph.

### Existing Rust production shadow

```text
ZEVRYON_RUST_UNICODE_AUTHORITATIVE=OFF
ZEVRYON_RUST_UNICODE_SHADOW=ON
ZEVRYON_RUST_UNICODE_SHADOW_STRICT=ON
```

C++ remains authoritative and Rust verifies it.

### Rust authority with C++ reverse shadow

```text
ZEVRYON_RUST_UNICODE_AUTHORITATIVE=ON
ZEVRYON_RUST_UNICODE_SHADOW_STRICT=ON
ZEVRYON_RUST_UNICODE_AUTHORITY_TEST_HOOKS=OFF
```

Enabling authority automatically selects the Rust core and Unicode FFI. CMake removes the original C++ translation unit from the target graph and substitutes `src/unicode_stream_authoritative.cpp`.

The authority translation unit reuses the unchanged C++ decoder implementation under a private namespace. This prevents algorithm drift while ensuring the private instance cannot provide browser-visible results.

## Output budget

The public PMR vector is reserved before the Rust state advances.

- If the public resource accepts the complete operation bound, Rust writes to a caller-owned temporary buffer and those records are committed without another PMR allocation.
- If the reserve fails, Rust is called with zero output capacity and therefore produces the authoritative `OutputBudgetExceeded/output_capacity` state at the first required emission.
- The C++ reverse shadow receives an equivalent null or unrestricted memory resource solely for parity checking.

A Rust internal output-buffer allocation failure is not eligible for C++ fallback and terminates fail-closed.

## Reverse-shadow comparison surface

Every authoritative operation compares Rust against the private C++ instance for:

- operation result;
- output count and every output field;
- error kind;
- error source offset;
- exact human-readable error message;
- all statistics;
- next source offset;
- failed state;
- reset result.

Message mismatches use the `error_message` telemetry class and retain deterministic 64-bit fingerprints for the expected and actual strings.

## Reverse-shadow fault proof

Diagnostic authority builds may enable:

```text
ZEVRYON_RUST_UNICODE_AUTHORITATIVE=ON
ZEVRYON_RUST_UNICODE_SHADOW_STRICT=OFF
ZEVRYON_RUST_UNICODE_AUTHORITY_TEST_HOOKS=ON
```

Four independent values of `ZEVRYON_UTF8_AUTHORITY_CPP_FAULT` corrupt only the private C++ comparison surface:

| Fault | Expected first mismatch |
|---|---|
| `output` | `output_record` |
| `error` | `error_kind` |
| `state` | `statistics` |
| `reset` | `reset_result` |

The native test requires the Rust public output, error message and authority identity to remain unchanged while the mismatch is detected and latched.

Authority hooks are rejected in strict mode and are never part of default production builds.

## Telemetry

Authority telemetry uses:

```text
schema: zevryon.rust-unicode-authority.v1
authoritative_backend: rust
reverse_shadow_backend: cpp
fallback_permitted: false
```

It preserves saturating operation, verification and mismatch counters plus the first mismatch class, index, expected value and actual value.

## Portable exact-head validation

The complete per-platform gate is implemented by:

```text
scripts/z2r3e_validate_unicode_authority.py
```

Example Linux invocation:

```text
python scripts/z2r3e_validate_unicode_authority.py \
  --sha <exact-product-head> \
  --platform linux \
  --compiler gcc-release
```

The harness accepts only the supported `linux` and `windows` platform identities, requires the declared platform to match the actual host OS, fails closed unless the checkout matches the supplied SHA, and performs:

1. Rust formatting, workspace tests and Clippy with warnings denied;
2. default Cargo-free C++ rollback configure/build/test;
3. strict Rust-authoritative configure/build/test;
4. diagnostic `output`, `error`, `state` and `reset` verifier-fault tests;
5. Unicode stream, grapheme, script-run and bidi-explicit regressions;
6. a structured `zevryon.z2r3eu.authority-validation.v1` JSON result and command logs.

The external Linux and Windows wrappers clone the repository without binding validation to a historical work branch, fetch the requested exact commit SHA, detach to that SHA and preserve platform evidence plus a SHA-256 package digest.

## Mandatory certification before merge

This implementation slice must not be considered certified until all of the following are executed on its exact product head:

- the portable validation harness passes on Linux and Windows;
- exact UTF-8 ABI `1.1` kind/detail/message tests pass;
- deterministic multilingual and malformed corpus parity is reconfirmed;
- baseline versus authority latency and RSS/PSS are measured;
- a final SHA-256-bound supported-platform authority-readiness manifest covering Windows and Linux is produced.

macOS is outside the supported desktop certification scope and must not be counted as a missing Z2R-3E-U gate.

## Rollback

Immediate rollback requires only build-option changes:

```text
ZEVRYON_RUST_UNICODE_AUTHORITATIVE=OFF
ZEVRYON_RUST_UNICODE_SHADOW=OFF
ZEVRYON_ENABLE_RUST_CORE=OFF
```

No source revert or disk-format migration is required.
