# Z2R-3C — Production MassiveDoc descriptor codec shadow

## Objective

Integrate the certified Rust MassiveDoc descriptor codec into the real production `StoreWriter` and `StoreReader` paths as an exact, non-authoritative shadow.

C++ remains authoritative for disk bytes, parsed descriptors, I/O, CRC32, SHA-256, manifests and search indexes. Rust receives the same real descriptor operations and verifies exact parity.

## Build modes

### Default rollback mode

```text
ZEVRYON_ENABLE_RUST_CORE=OFF
ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW=OFF
ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW_STRICT=OFF
```

The production MassiveDoc graph remains C++ only. Cargo is not required and the Rust MassiveDoc static library is not built or linked.

### Diagnostic shadow mode

```text
ZEVRYON_ENABLE_RUST_CORE=ON
ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW=ON
ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW_STRICT=OFF
```

C++ output and parsed values remain authoritative. Rust performs the same codec operation, records checks and latches the first mismatch without changing the browser result.

### Strict certification mode

```text
ZEVRYON_ENABLE_RUST_CORE=ON
ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW=ON
ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW_STRICT=ON
```

The first mismatch terminates the certification process after recording the failing operation class.

## Production observation points

The shadow wraps the existing internal helpers after their definitions and before all production call sites:

- record descriptor serialization during `StoreWriter::append_stream()`;
- chunk descriptor serialization during segment/chunk publication;
- record descriptor parsing in `StoreReader`;
- chunk descriptor parsing in `StoreReader`.

The wrapper calls the existing C++ helper first. That C++ result remains the value written to disk or returned to the caller. Rust then verifies:

- complete 32-byte record encoding;
- all decoded record fields;
- complete 24-byte chunk encoding;
- all decoded chunk fields;
- canonical zero reserved chunk field after Rust decoding.

## Telemetry

`MassiveDocDescriptorShadowSnapshot` reports:

- record encode checks;
- record decode checks;
- chunk encode checks;
- chunk decode checks;
- mismatch count;
- first mismatch operation.

Mismatch classes are:

```text
RecordEncode
RecordDecode
ChunkEncode
ChunkDecode
```

Counters are process-wide atomics because production readers and writers may be created independently. They are diagnostic only and do not affect disk behavior.

## Real workload certification

The focused test creates two records with a 32-byte segment limit so both payloads cross multiple real segment boundaries. It then:

1. writes and finalizes the store;
2. opens the generated manifest and indexes;
3. verifies every record CRC and the complete payload SHA-256;
4. reads a bounded slice spanning chunk boundaries;
5. confirms every real descriptor encode/decode was observed;
6. requires zero Rust mismatches.

The diagnostic test supplies an intentionally incorrect C++ record byte array directly to the verifier and proves that `RecordEncode` divergence is counted and latched.

## Cross-platform gates

Ubuntu, Windows and macOS strict jobs require:

- Rust formatting;
- complete combined Rust workspace tests;
- Clippy with warnings denied;
- strict production shadow compilation;
- real writer/reader codec-shadow execution;
- unchanged MassiveDoc store tests.

An Ubuntu diagnostic job requires both positive parity and negative divergence detection. A separate Ubuntu rollback job configures Rust completely off and proves that MassiveDoc builds and tests without Cargo in the production graph.

## Authority boundary

This slice does not promote Rust to MassiveDoc authority. Specifically, Rust does not control:

- bytes written to `records.idx` or `chunks.idx`;
- parsed descriptors returned by the reader;
- record/chunk table seeking;
- segment I/O;
- CRC32 or SHA-256;
- manifest validation;
- search signatures;
- error strings or external API behavior.

Promotion requires broader real-corpus and giant-record shadow evidence, performance/RSS measurement, exact exported-payload parity and an immediate build-time rollback path.
