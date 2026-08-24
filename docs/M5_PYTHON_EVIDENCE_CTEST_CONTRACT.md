# M5 — Python Evidence Contract in CTest

## Goal

The native frame-certification path includes Python evidence code, so the normal CTest graph must exercise its schema and fail-closed semantics without making `pytest` a build dependency.

## Build integration

When `BUILD_TESTING` is enabled, CMake performs a quiet `Python3` interpreter lookup. If an interpreter is present, CTest registers `python-frame-evidence-contract-smoke`. If Python is absent, native C++ builds remain available and no new mandatory package is imposed on embedders.

The smoke test uses only the Python standard library plus Zevryon's in-tree modules.

## Contract covered

The deterministic smoke test requires:

- 16 GiB RAM metadata maps to the canonical `desktop` device profile;
- explicit physical-device confirmation plus nominal thermal evidence and 1000 low-latency samples certifies frame latency;
- nearest-rank P99 remains exact for the deterministic sample;
- the sample digest remains a 64-character SHA-256 value;
- serialized frame evidence retains `frame_latency_certified=true`;
- a valid native probe envelope produces `native_frame_certified=true`;
- missing physical-device confirmation fails closed;
- `serious` thermal state fails closed;
- a prefetch worker-bound violation (`pool_thread_starts=65`) fails native certification.

This is a schema/logic contract test. It is not a physical benchmark and does not replace the one-command physical certification harness.
