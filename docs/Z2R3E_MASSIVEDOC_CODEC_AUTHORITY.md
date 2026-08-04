# Z2R-3E MassiveDoc Descriptor Authority

## Scope

Z2R-3E promotes only the fixed-size MassiveDoc record and chunk descriptor codec from certified production Rust shadow execution to an explicit Rust-authoritative mode.

The authority switch is opt-in. The default build remains the existing C++ production implementation and does not require Cargo.

The following components remain outside this slice and remain C++ authority:

- record and chunk table seeking;
- segment file I/O;
- CRC32 and payload SHA-256;
- store manifests and search signatures;
- corpus import/export orchestration;
- error strings and external `StoreWriter`/`StoreReader` API behavior.

## Prerequisite

This slice is stacked on the exact Z2R-3D certification:

```text
Z2R-3D head:
acb7967ce905ba43215e09716e414584d7c790da

Z2R-3D promotion-readiness manifest SHA-256:
8ec0438fd621fa9770439d3b03d19cf48c05eeb84eae33cc1a4ec548e75d6a05
```

The final Z2R-3E manifest rejects any other prerequisite head or manifest digest.

## Build modes

### Unchanged C++ production

```text
ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE=OFF
ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW=OFF
ZEVRYON_ENABLE_RUST_CORE=OFF
```

Rust is absent from the MassiveDoc production graph. The existing C++ codec supplies disk bytes and parsed descriptor values.

### Rust production shadow

```text
ZEVRYON_ENABLE_RUST_CORE=ON
ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW=ON
ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE=OFF
```

C++ remains authoritative and Rust verifies exact bytes and values.

### Rust authority with C++ reverse shadow

```text
ZEVRYON_ENABLE_RUST_CORE=ON
ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW=ON
ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE=ON
ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW_STRICT=ON
```

Rust supplies:

- all 32 record-descriptor bytes written to `records.idx`;
- all 24 chunk-descriptor bytes written to `chunks.idx`;
- all record descriptor fields returned by parsing;
- all chunk descriptor fields returned by parsing.

C++ executes the same codec operation as a private reverse shadow. Exact bytes or parsed fields are compared after every operation.

## Fail-closed authority behavior

Rust authority does not silently fall back to C++.

- ABI version or descriptor-size contract failure terminates authority execution.
- Rust encode failure terminates authority execution.
- Rust decode failure returns a decode failure even if C++ produced a value.
- A Rust/C++ divergence is latched with the first operation class.
- Strict certification mode terminates immediately on any divergence.
- Diagnostic mode retains the Rust result while exposing the C++ reverse-shadow mismatch.

Mismatch classes remain:

```text
RecordEncode
RecordDecode
ChunkEncode
ChunkDecode
```

## Adversarial proof

The dedicated authority test can corrupt only the C++ reverse-shadow comparison for record encode and record decode.

The test requires all of the following simultaneously:

- the mismatch is counted and latched;
- the authoritative backend remains Rust;
- Rust-authored disk bytes remain readable;
- the complete record payload remains unchanged;
- Rust-decoded public output remains unchanged.

This distinguishes real authority from a shadow-only integration.

## Mandatory certification

Z2R-3E runs:

- strict Rust authority on Ubuntu, Windows and macOS;
- exact store round-trip and descriptor telemetry;
- Ubuntu C++ reverse-shadow corruption;
- combined Rust ResourceLedger plus Rust descriptor authority;
- unchanged Cargo-free C++ rollback;
- paired C++ baseline versus Rust authority on each platform;
- a deterministic 128 MiB corpus;
- 131,072 records;
- one real 64 MiB record;
- 16 MiB segments;
- three paired samples with alternating execution order;
- exact recursive store path, size and SHA-256 parity;
- exact exported payload size and SHA-256 parity;
- exact import, open, verify and export semantic parity;
- performance and RSS/PSS gates inherited from Z2R-3D;
- final cross-platform authority-readiness manifest.

## Rollback

Immediate rollback requires no source revert:

```text
ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE=OFF
ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW=OFF
ZEVRYON_ENABLE_RUST_CORE=OFF
```

The rollback workflow verifies that the Cargo target directory and Rust authority test target are absent from the C++ production graph.
