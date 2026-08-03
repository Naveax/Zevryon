# Z2R-1 — Ledger performance oracle and shadow execution

## Objective

Z2R-1 measures the real cost of crossing the C ABI into the Rust logical core and introduces a production-safe shadow mode before Rust is allowed to make browser decisions.

The C++ `ResourceLedger` remains authoritative in this slice. Every mutation is replayed into the Rust ledger. Divergence is diagnostic and latched; it does not alter the result returned to the current browser core.

## Shadow contract

`ShadowResourceLedger` owns:

- one authoritative C++ `ResourceLedger`;
- one Rust `RustResourceLedger` using caller-owned fixed ABI storage;
- a configurable full-verification interval;
- saturating operation, verification and mismatch counters;
- the first mismatch classification, resource class, snapshot field, expected value and actual value.

The following are compared exactly:

- operation results for reservations;
- all 12 fields of every one of the 36 resource snapshots;
- aggregate current and peak bytes;
- hard-limit status;
- accounting-clean status.

The default interval is 1,024 mutations. Production callers can set the interval to zero for manual checkpoint verification or choose a larger interval when measuring overhead.

## Performance oracle

The benchmark uses one pre-generated deterministic operation stream with:

- accepted and rejected reservations;
- releases and intentional over-release paths;
- cache hits, misses and evictions;
- saturating physical read and write accounting;
- all 36 resource classes.

The generator is outside the timed section. Each implementation replays the exact same 300,000 operations for 13 rotated samples after warm-up.

Reported implementations:

1. authoritative C++ ledger;
2. Rust ledger through the stable C ABI;
3. C++ authoritative plus Rust shadow with a 4,096-operation full-verification interval.

The report schema is `zevryon.ledger-performance.v1` and records p50, p95, p99, maximum nanoseconds per operation, operations per second and p50 ratios.

Initial gates are deliberately catastrophic-regression gates, not promotion gates. Hosted runners are allowed broad scheduling variance. Tighter platform-specific gates must be derived from collected artifacts rather than invented before measurement.

## FFI hot-path correction

The C++ Rust wrapper no longer calls `zr_ledger_valid` before every operation. The mutating FFI function already validates the ledger magic and fails closed. The wrapper now checks its construction state and performs one FFI call per ledger operation.

This removes a redundant language-boundary crossing while preserving the same failure behavior.

## Promotion requirements

Rust must not replace the production ledger until all of the following are true:

- exact shadow mismatch count remains zero under representative browser workloads;
- Linux, Windows and macOS benchmark baselines are recorded;
- no unacceptable p95 or p99 regression exists;
- sanitizer and ABI certification remain green;
- production root-CMake integration has an immediate rollback option;
- fault injection proves that divergence is detected and surfaced.

## Deliberate exclusions

This slice does not:

- remove the C++ ledger;
- change the native GPU or operating-system backends;
- make Rust authoritative;
- claim that Rust is faster before benchmark artifacts exist.
