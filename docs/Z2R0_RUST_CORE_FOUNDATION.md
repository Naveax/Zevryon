# Z2R-0 Rust Core Foundation

Z2R-0 begins the incremental migration of Zevryon's logical browser core from C++ to Rust without discarding the certified native GPU and operating-system backends.

## Boundary

Rust owns the first safety-critical logical component: resource accounting. C++ remains the certified production implementation until the Rust implementation proves exact behavioral equivalence and acceptable performance.

The initial language boundary is a fixed C ABI:

- no C++ STL or Rust collection crosses the boundary;
- no allocation is transferred between languages;
- the caller owns one 4,096-byte, 8-byte-aligned ledger storage record;
- every resource class is represented by the existing stable numeric order;
- invalid, uninitialized, null, or misaligned inputs fail closed;
- physical I/O counters saturate exactly as the C++ oracle does;
- over-release records an accounting error and clears the affected current allocation;
- all snapshots use fixed-width counters and native `size_t` byte fields.

## Workspace

The Rust workspace contains:

- `zevryon-abi`: C-compatible records and ABI constants;
- `zevryon-ledger`: safe Rust resource-ledger implementation;
- `zevryon-ffi`: audited raw-pointer boundary and static library exports.

The logical ledger implementation contains no `unsafe` code. Unsafe operations are isolated to the FFI crate and limited to validated fixed-storage casts and output writes.

## Certification

The C++ equivalence test executes the same sequence against the existing C++ `ResourceLedger` and the Rust implementation, then compares:

- all 36 resource snapshots;
- hard limits, current bytes, and peak bytes;
- successful and rejected reservations;
- release and accounting-error counts;
- cache hit, miss, and eviction counts;
- saturating physical read and write counters;
- aggregate current and peak bytes;
- hard-limit and accounting-clean status.

The standalone bridge build deliberately avoids modifying the production root CMake graph in this first slice. Once Linux, Windows, and macOS equivalence certification is green, the bridge can be promoted into the main build behind a controlled feature option.

## Migration policy

A C++ component may be retired only after its Rust replacement satisfies all of the following:

1. exact oracle equivalence;
2. no increase in bounded peak memory;
3. no material latency regression;
4. cross-platform strict-warning builds;
5. Rust formatting, tests, and Clippy with warnings denied;
6. a documented and counted unsafe surface;
7. failure-atomic behavior for every rejected operation.

Native D3D12, Vulkan, Metal, DirectWrite, CoreText, Fontconfig, and operating-system window integrations remain outside this migration slice.
