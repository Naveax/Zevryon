# Z2R-2 — ResourceLedger authority promotion

## Objective

Z2R-2 promotes the certified Rust `ResourceLedger` from production shadow execution to an explicit, build-time authoritative backend. The C++ implementation remains in the same process as an exact reverse shadow and rollback implementation.

The public C++ `ResourceLedger` API does not change. Existing MassiveDoc, PMR, layout, Unicode, font, raster, network, compositor and JavaScript accounting call sites continue to use the same class and methods.

## Build modes

### Default C++ authority

```text
ZEVRYON_RUST_LEDGER_AUTHORITATIVE=OFF
ZEVRYON_ENABLE_RUST_CORE=OFF
ZEVRYON_RUST_LEDGER_SHADOW=OFF
```

This is the unchanged default. `src/resource_ledger.cpp` is compiled, Cargo is not required, Rust is not linked and C++ remains authoritative.

### Certified C++ authority with Rust shadow

```text
ZEVRYON_ENABLE_RUST_CORE=ON
ZEVRYON_RUST_LEDGER_SHADOW=ON
ZEVRYON_RUST_LEDGER_AUTHORITATIVE=OFF
```

This retains the Z2R-1C/Z2R-1D boundary: C++ decides and Rust verifies.

### Rust authority with C++ reverse shadow

```text
ZEVRYON_RUST_LEDGER_AUTHORITATIVE=ON
ZEVRYON_RUST_LEDGER_SHADOW_STRICT=ON
ZEVRYON_RUST_LEDGER_SHADOW_INTERVAL=64
```

The authority option selects the Rust core and production mirror automatically. The production target substitutes `src/resource_ledger_authoritative.cpp` for the existing C++-authoritative translation unit.

In this mode:

- Rust returns reservation decisions;
- public snapshots and aggregate accounting are read from Rust;
- C++ replays every operation as a reverse shadow;
- all 12 fields across all 36 resource classes are compared;
- aggregate current bytes, peak bytes, hard-limit status and accounting-clean status are compared;
- the first mismatch is latched with expected Rust value and actual C++ value;
- strict mode aborts immediately on divergence;
- Rust initialization, ABI-version or resource-class-count failure always aborts because no alternate authority may be selected silently.

## Authority proof

The dedicated authority test performs two independent certifications.

### Positive certification

- all 36 resource classes;
- exact-limit acceptance and one-byte-over-limit rejection;
- releases, cache hits, cache misses and evictions;
- saturating physical I/O accounting;
- public Rust-sourced snapshots and aggregates;
- real `LedgerMemoryResource` / PMR allocation and deallocation;
- exact C++ reverse-shadow verification;
- zero mismatches and authority telemetry.

### Negative divergence certification

A test-only hook mutates only the private C++ reverse-shadow state. The test then proves:

- the public snapshot remains the unchanged Rust value;
- the public aggregate remains the unchanged Rust value;
- exact verification detects the C++ divergence;
- telemetry records Rust as authoritative and C++ as the verifier;
- the first mismatch records the Rust expected value and corrupted C++ actual value.

## Telemetry

The existing embedded schema remains `zevryon.rust-shadow-ledger.v1` for compatibility and adds authority identity:

```json
{
  "mode": "rust_authoritative",
  "authoritative": true,
  "authoritative_backend": "rust",
  "shadow_backend": "cpp"
}
```

The outer `zevryon.resource-ledger.v1` report also records:

```json
{
  "authoritative_backend": "rust",
  "verification_backend": "cpp"
}
```

## Rollback

Rollback is a build-only change and does not require source reversion:

```text
ZEVRYON_RUST_LEDGER_AUTHORITATIVE=OFF
ZEVRYON_ENABLE_RUST_CORE=OFF
ZEVRYON_RUST_LEDGER_SHADOW=OFF
```

The root target then compiles the original `src/resource_ledger.cpp` path and has no Rust build or link dependency.

## Deliberate boundary

This promotion applies only to `ResourceLedger` authority. It does not move D3D12, Vulkan, Metal, DirectWrite, CoreText, Fontconfig, HarfBuzz or operating-system ownership into Rust. Native GPU and platform adapters remain C++/Objective-C++ boundaries.
