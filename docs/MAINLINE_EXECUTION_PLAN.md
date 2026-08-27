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
- [x] Add physical-device benchmark metadata and thermal state capture.

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

- Windows: preserve host-memory fallback; capture `LowMemoryResourceNotification` and immediate-job accounting/limit telemetry; use the OS low-memory signal only as a conservative pressure floor. Do not infer an effective nested-job memory domain from the immediate job alone.
- Linux: use cgroup v2 effective memory-domain data and PSI where available with procfs fallback. Existing adaptive bounded sampling provides polling at 1000 ms Normal / 250 ms Elevated / 100 ms Critical, so no duplicate polling thread is required.
- Android: provide a native trim-memory / low-RAM policy contract. Java/Kotlin/JNI callback wiring remains an integration boundary until an Android application shell exists in this repository.
- Apple platforms: intentionally unsupported under the current target policy. The Apple backend removal guard is authoritative; do not reintroduce a macOS/iOS memory-pressure backend through M6.
- 32-bit: preserve 64-bit file positions while enforcing bounded positional-I/O, mapped-window and record-materialization limits appropriate to the process address space.
- Portability: keep scalar exact matching as the correctness authority and use SIMD only as an optional runtime-selected acceleration backend.

## M7 — Competitor laboratory

Run the same corpus and operations in Zevryon, Chrome, Firefox, Edge, WebKit, Servo and Ladybird where supported. Publish raw runs, medians, P95/P99, corpus hashes, system state and failure modes.

No leadership claim is allowed until Zevryon is first in at least four core efficiency metrics and within 5% of the leader in every remaining core metric.

Implementation/admission state:

- [x] Freeze the five lower-is-better core metrics and the 4-of-5 / remaining-within-5% ranking rule before final evidence collection.
- [x] Define common setup, warmup, query-timing and case-owned process-tree memory boundaries.
- [x] Bind warmup count and execution semantics into the canonical scenario fingerprint.
- [x] Wire exact branded Chrome/Edge Playwright channels and exact Servo/Ladybird WebDriver identities without substitution.
- [x] Normalize browser setup timing from pre-launch case start through post-warmup ready.
- [x] Add persistent Zevryon benchmark-session execution with implementation-local raw query timing.
- [x] Build the Zevryon case-owned canonical synthetic store after process launch and enforce exact 1 MiB chunk restart corpus semantics.
- [x] Attach and revalidate raw normalized setup/query/memory evidence for successful cases.
- [x] Add a legacy-independent exact canonical six-browser x two-mode normalized full-set collector.
- [x] Add the separate machine-readable five-metric leadership evaluator; collection alone remains unable to claim leadership.
- [x] Admit exact-runtime preflight, stable preflight-to-measurement runtime identity binding, collection admission and single-bundle/no-cherry-pick discipline through exact-head run `33122221313` on commit `6ea7a74123069dbdb035bd59cf93a3f870f85a9d`.
- [ ] Admit the physical-host/system-fingerprint/physical-Zevryon/publication-manifest candidate after exact-head run `33125276373` completes successfully on commit `fcd211776675993a8ce7ad0954f2134b24389143`.
- [ ] Admit the follow-on raw-artifact admission-replay and artifact-root-containment candidate after its own exact-head CI success; parent CI does not validate this child.
- [ ] Run the real six-runtime readiness preflight on the final physical benchmark host and preserve its M0 machine/thermal evidence artifact.
- [ ] Collect one complete canonical 6x2 browser evidence bundle on that same physical system with no runtime substitution and observed thermal evidence.
- [ ] Collect Zevryon `virtualized` and `native-dom` evidence through `m7_zevryon_physical_case.py`, preserving certified M0 machine/thermal receipts immediately before and after each normalized case.
- [ ] Admit the four artifacts through the collection binder, including preflight/browser physical-host certification, both Zevryon before/after physical receipts, artifact SHA-256 receipts and stable runtime identity checks.
- [ ] Evaluate the fixed five-metric rule on the admitted bundle. Exit status `2` means valid evidence that does not satisfy leadership, not a harness failure.
- [ ] Create the canonical publication manifest only after re-hashing and re-reading all four raw artifacts, replaying `admit_collection()`, constraining artifact paths to `artifact_root`, verifying exact clean Git commit/tree, and binding physical-host receipts, runtime identities and evaluator result.
- [ ] Publish the complete admitted evidence and failure modes without cherry-picking metrics across repeat bundles.

One leadership decision consumes one complete evidence bundle. Repeats may be collected as independent reproducibility evidence, but best-of-N reruns and cross-bundle metric mixing are not admissible. Any repeated-run aggregation policy must be frozen before collecting the evidence it would aggregate.

## M8 — 100/100 gate

- Full certified adversarial envelope.
- Every device profile within its hard cap.
- 24-hour soak.
- Ten million mixed mutations.
- Crash injection during every storage transaction stage.
- Unicode, serializer, index and sequence property fuzzing.
- Zero crash/OOM, data corruption, invalid UTF-8 output or logical-order mismatch.
