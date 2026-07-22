# ZENITH Master Program — Z0 to Z15

## Status

This document is an execution contract, not a claim that a complete browser already exists.

| Milestone | Current state |
|---|---|
| Z0 — resource ledger and benchmark contract | Active in `work/z0-resource-ledger` |
| Z1–Z15 | Planned and dependency-gated |

The machine-readable source of truth is `config/zenith_program.json`. CI rejects missing milestones, dependency cycles, skipped implemented dependencies, evidence-free completion claims, and weakened MassiveDoc core gates.

## Immutable performance floor

Feature work may add shaping, style, layout, paint, JavaScript, networking, accessibility, or security cost. It may not hide a regression in the existing storage and viewport core.

| Gate | Maximum |
|---|---:|
| Random core viewport P95 | 0.50 ms |
| Adjacent core viewport P95 | 0.30 ms |
| Random core viewport P99 | 0.75 ms |
| Physical source read per query | 65,536 bytes |
| Persistent checkpoint overhead | 0.20% |
| Silent corruption | 0 |
| Payload data loss | 0 bytes |

## Z0 deliverables

Z0 establishes the controls required before browser-scale subsystems are added:

1. a fixed resource-class registry;
2. allocation-before-use hard-cap reservations;
3. current, peak, rejected, release, hit, miss, eviction, and physical-I/O counters;
4. overflow-safe and accounting-error-visible counters;
5. one benchmark schema carrying commit, environment, workload hash, cold/warm/hot mode, latency distribution, PSS, I/O, resource budgets, correctness, and gates;
6. a Z0–Z15 dependency graph validated on every build;
7. conversion of the existing giant-record evidence into the unified report.

`ResourceLedger` is deliberately a single-owner primitive. It contains no hidden allocation and no internal mutex. A process or worker may own one ledger; later cross-process aggregation must merge snapshots rather than place a lock in viewport hot paths.

## Program dependency graph

```text
Z0 Resource/benchmark contract
├── Z1 Unicode substrate
│   └── Z2 Font/shaping
│       ├── Z4 Incremental layout
│       │   ├── Z5 Retained paint
│       │   │   └── Z6 Compositor/GPU
│       │   └── Z14 Accessibility/input
│       └── Z14 Accessibility/input
├── Z7 HTML parser
│   └── Z8 Logical DOM
│       ├── Z3 Style/cascade
│       │   └── Z4 Incremental layout
│       ├── Z9 JavaScript/Web IDL
│       ├── Z10 Mutation/WAL
│       └── Z14 Accessibility/input
├── Z13 Process isolation/sandbox
│   ├── Z6 Compositor/GPU
│   ├── Z9 JavaScript/Web IDL
│   └── Z12 Network/platform services
└── Z11 Live100 depends on Z6, Z9, Z10, and Z12
    └── Z15 Compatibility/adaptive/product depends on Z6, Z9, Z11, Z12, Z13, Z14
```

## Completion semantics

A milestone can have only one of three states:

- `planned`: design exists, no completion evidence is claimed;
- `active`: all dependencies are implemented and this is the current single-owner workstream;
- `implemented`: every dependency is implemented and evidence entries identify reproducible CI artifacts.

A milestone is not implemented merely because interfaces or placeholder classes exist. Empty stubs, unconditional PASS flags, disabled tests, and synthetic competitor scores are prohibited.

## Merge policy

A PR is rejected when any of these apply:

- no correctness test;
- no before/after or contract evidence for a performance-sensitive change;
- an unbounded resident cache;
- a new full-document scan in a viewport path;
- silent fallback after corruption;
- source payload mutation;
- hidden-page paint or raster without an explicit realtime exemption;
- cross-origin immutable sharing without partitioning analysis;
- a weakened core gate;
- a strict build or sanitizer failure;
- a milestone marked implemented without evidence.

## Why Z0 must precede Z1–Z15

Without a shared resource ledger and benchmark contract, later subsystems can move cost between heap, mapped pages, GPU memory, page cache, I/O, or another process and appear faster without becoming more efficient. Z0 makes those transfers visible before Unicode, CSS, rendering, JavaScript, networking, and sandbox code enlarge the system.
