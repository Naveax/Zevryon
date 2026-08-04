# Z2R-3D — MassiveDoc descriptor codec promotion readiness

## Objective

Certify whether the Rust MassiveDoc record/chunk descriptor codec is ready to become an authority candidate after Z2R-3C production shadow integration.

This slice does **not** switch production authority. C++ continues to produce the bytes written to disk and the descriptors returned by readers. Rust remains an exact observer while Z2R-3D measures real-corpus parity, overhead, memory and fault behavior on Linux, Windows and macOS.

## Prerequisite

Z2R-3D is stacked on the certified Z2R-3C head:

```text
0fb78d6f598f96eff1e9df6fde6897442e0ac443
```

The final manifest rejects a missing or changed prerequisite SHA.

## Paired production builds

Each platform builds the same injected workload target twice.

### Authoritative baseline

```text
ZEVRYON_ENABLE_RUST_CORE=OFF
ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW=OFF
ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW_STRICT=OFF
```

The target links the normal C++ MassiveDoc production core. Cargo and the Rust MassiveDoc codec are absent from the execution graph.

### Rust descriptor shadow

```text
ZEVRYON_ENABLE_RUST_CORE=ON
ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW=ON
ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW_STRICT=OFF
```

Non-strict mode is required only so the four deliberate fault-injection processes can report their telemetry instead of aborting. Every positive workload requires zero mismatches and `first_mismatch=None`.

## Deterministic real corpus

Each paired sample creates a store through the real `StoreWriter::append_stream()` path with:

- 128 MiB logical payload;
- 131,072 records;
- 16 MiB segment limit;
- one real 64 MiB record;
- deterministic byte generation independent of platform;
- at least one chunk descriptor per record, plus additional descriptors for the giant record.

Three independent paired samples are required. Execution order alternates between baseline-first and shadow-first to reduce systematic hosted-runner bias.

## Exact parity requirements

For every import sample, the baseline and shadow stores are recursively inventoried. Every relative path, file size and file SHA-256 must match. A canonical tree SHA-256 binds the complete inventory.

For every export sample:

- exported byte counts must equal the 128 MiB logical payload;
- baseline and shadow exported SHA-256 values must match;
- the exported SHA-256 must equal the payload SHA-256 stored in the MassiveDoc manifest.

Operation JSON is normalized by removing only mode, timing and shadow telemetry. The remaining semantic output must have an exact SHA-256 match.

## Exact descriptor telemetry

The shadow process resets telemetry before each operation and must observe the following exact counts:

| Operation | Record encode | Record decode | Chunk encode | Chunk decode |
|---|---:|---:|---:|---:|
| `import` | records | 0 | chunks | 0 |
| `open` | 0 | 0 | 0 | 0 |
| `verify` | 0 | records × 2 | 0 | chunks |
| `export` | 0 | records | 0 | chunks |

`verify` reads each record descriptor once for integrity metadata and once through `read_record()`. `open` intentionally performs no descriptor parsing; it validates the manifest, index sizes and segment inventory.

The canonical chunk count is derived only from successful import encode telemetry. Verify and export telemetry are checked against that independent value, preventing a forged reader report from defining its own expected count.

## Fault classes

Four separate processes deliberately inject one divergence each:

```text
RecordEncode
RecordDecode
ChunkEncode
ChunkDecode
```

Each process must report:

- exactly one mismatch;
- the expected first-mismatch class;
- exactly one operation counter in the injected class;
- zero counters in the other three classes.

## Performance measurements

For `import`, `open`, `verify` and `export`, the paired runner records three samples of:

- external wall time;
- workload-internal elapsed time;
- peak RSS;
- peak PSS where the operating system exposes it.

The report includes P50, P95, P99 and maximum wall/internal distributions.

Platform certification uses fail-closed ratio-or-absolute-delta gates:

| Metric | Ratio gate | Absolute delta gate |
|---|---:|---:|
| P50 wall | 2.00× | 5 s |
| P95 wall | 2.25× | 5 s |
| P99 wall | 2.50× | 5 s |
| Maximum wall | 3.00× | 5 s |
| Peak memory | 1.50× | 16 MiB |
| Total P50 wall | 2.00× | 10 s |

Linux uses peak PSS. Windows and macOS use peak RSS because portable PSS is not available there.

The absolute-delta branch prevents a fixed Rust runtime footprint from failing solely because a very short baseline process has a small denominator. Both ratio and absolute delta must exceed their limits for a gate to fail.

## Canonical evidence chain

Every paired report includes a canonical SHA-256 over its complete content. Platform certification recomputes that hash before accepting any field.

Every platform manifest includes its own canonical SHA-256. Finalization recomputes all three platform hashes and then requires:

- the same tested commit;
- identical workload parameters;
- identical payload SHA-256;
- identical store-tree SHA-256 and file count;
- identical semantic hashes for all four operations;
- successful platform performance/memory gates;
- zero positive-workload mismatches;
- detection of all four negative fault classes.

The final promotion-readiness manifest binds Linux, Windows and macOS evidence into one SHA-256 chain.

## Authority and rollback

A successful Z2R-3D result means the Rust descriptor codec is a promotion candidate. It does not itself make Rust authoritative.

The final manifest explicitly records:

```text
authoritative_switch_performed=false
current_authoritative_backend=cpp
promotion_candidate=rust-massivedoc-descriptor-codec
rollback_retained=true
```

Immediate rollback remains:

```text
ZEVRYON_ENABLE_RUST_CORE=OFF
ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW=OFF
```

The subsequent authority-switch slice must separately replace the C++ descriptor result, retain reverse-shadow comparison, preserve exact disk bytes and certify rollback again.
