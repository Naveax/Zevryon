# MassiveDoc Mainline Execution Plan

## Rule zero

No optimization is accepted if it reduces document correctness. Full selection, copy, search, export, source-byte fidelity and semantic order are invariants.

## M0 — Contract, naming and measurement

- [x] Rename the public project surface to Zevryon.
- [x] Pause release numbering.
- [x] Replace message-count-only targets with a multi-axis content envelope.
- [x] Define legacy/mid/modern/desktop memory and latency profiles.
- [x] Add a deterministic streaming corpus writer.
- [x] Add a no-compensation 100/100 evaluator.
- [x] Bound image/math shell caches by device class.
- [x] Add Linux process-group PSS sampling; Windows and Android backends remain.
- [ ] Add physical-device benchmark metadata and thermal state capture.

## M1 — Native segmented source and bounded process control

- [x] Store source text in immutable segmented files.
- [x] Use 64-bit global byte positions and fixed-width record/chunk descriptors.
- [x] Support records that span segment boundaries.
- [x] Add per-record CRC32 and full-payload SHA-256 verification.
- [x] Add bounded record-slice materialization and streaming export.
- [x] Add a disk-backed bigram block index with no false negatives.
- [x] Add Linux process-group PSS sampling and device-profile pressure states.
- [x] Integrate `MASSIVE_OPEN`, `MASSIVE_FIND`, `MASSIVE_RECORD`, and `MASSIVE_STATS` into the document protocol.
- [ ] Replace the remaining browser logical-node objects with a compact struct-of-arrays arena.
- [ ] Intern repeated tags, attributes, roles and styles.

Validated M1 evidence:

- 64 MiB / 131,072-record corpus imported, searched and fully verified.
- Tail marker search completed in 19.74 ms engine time.
- Peak measured native-store PSS stayed below 3.1 MB in that smoke run.
- Exact payload SHA-256 and record CRC invariants passed.

M1 is complete for the native source store. Compact browser-node integration continues as M2.

## M2 — Compact logical arena and chunked order-statistics sequence

- Replace the document-order vector and O(n) position map.
- Store subtree record counts, text bytes, layout height and search summaries.
- Support O(log n) access, offset lookup, insert, delete, move and height update.
- Add copy-on-write roots for snapshots and concurrent readers.
- Eliminate full-tree rebuilds from normal operation.

## M3 — Crash-safe segmented generations and mobile I/O

The initial segmented source store is implemented in M1. M3 hardens it for production-like recovery and low-end devices:

- Add configurable immutable content blocks and `pread`/windowed-I/O backends.
- Maintain 32-bit-process-safe windows.
- Implement hot/warm/cold admission and eviction.
- Add crash-safe generation manifests and append journal.
- Add background compaction and corruption quarantine.
- Preserve giant records as segmented ranges rather than materializing them.

Exit gates:

- 4 GiB corpus opens on the legacy profile without OOM.
- First viewport becomes usable before background import/index completion.
- Resident cold-store pages count against the measured PSS.

## M4 — Bounded search and full-document operations

- Block Bloom summaries.
- Compressed trigram postings stored on disk.
- SIMD candidate verification where available, scalar fallback everywhere.
- Bounded Unicode normalization and case-fold pipeline.
- O(1) full-document selection descriptor.
- Streaming text/HTML export with fixed memory.
- Cancellation at every block boundary.

## M5 — Frame-budget scheduler

- Device-profile-specific frame budget.
- Velocity-aware prefetch and cancellation.
- Visible layout first; background work receives leftover budget only.
- No blocking disk, compression, full traversal or image decode on the UI thread.
- Pressure controller shrinks cache before the operating system kills the process.

## M6 — Cross-platform low-memory backend

- Windows: memory-pressure notification and job-object accounting.
- Linux: cgroup/PSI awareness where available, procfs fallback.
- Android: trim-memory callbacks and low-RAM device mode.
- macOS/iOS: memory-pressure dispatch sources.
- 32-bit-safe file offsets and bounded address-space windows.
- Portable scalar algorithms with optional SIMD backends.

## M7 — Competitor laboratory

Run the same corpus and operations in Zevryon, Chrome, Firefox, Edge, WebKit, Servo and Ladybird where supported. Publish raw runs, medians, P95/P99, corpus hashes, system state and failure modes.

No leadership claim is allowed until Zevryon is first in at least four core efficiency metrics and within 5% of the leader in every remaining core metric.

## M8 — 100/100 gate

- Full certified adversarial envelope.
- Every device profile within its hard cap.
- 24-hour soak.
- Ten million mixed mutations.
- Crash injection during every storage transaction stage.
- Unicode, serializer, index and sequence property fuzzing.
- Zero crash/OOM, data corruption, invalid UTF-8 output or logical-order mismatch.
