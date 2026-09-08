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
- [x] Replace remaining browser logical-node heap objects with compact disk-backed logical-node arena authority.
- [x] Intern repeated tags, attributes, roles and styles out-of-line.

Validated M1 evidence:

- 64 MiB / 131,072-record corpus imported, searched and fully verified.
- Tail marker search completed in 19.74 ms engine time.
- Peak measured native-store PSS stayed below 3.1 MB in that smoke run.
- Exact payload SHA-256 and record CRC invariants passed.
- Browser logical-node arena/source/index infrastructure admitted through PR #158, exact head `4bee237c2263b139b10a29c03dd87956b90e2d5a`, exact-head CI `34237003555` SUCCESS, merged as `04990b0ff89c59063e761e147afa7069d06d4d9d`.
- Real `ZenithTabRuntime` source-record semantic adoption admitted through PR #159, exact head `31ed41801fca2a6b092e57274ee1f14d2deb10ee`, exact-head CI `34240329146` SUCCESS, merged as `add6d43b925dc94e85111b4a57007673f142ca79`.

M1 source-store and browser logical-node storage/interning adoption are complete. The 67,108,864-node certification envelope remains an M8 raw/physical evidence boundary and is not fabricated by this code-side admission.

## M2 — Compact logical arena and chunked order-statistics sequence

- [x] Replace the document-order vector and O(n) position map with a bounded chunked order-statistics sequence.
- [x] Store subtree record count, text-byte, layout-height and search-summary aggregates.
- [x] Support bounded logarithmic in-memory record/rank/offset lookup, insert, erase, move, height update and search-summary update.
- [x] Add copy-on-write roots with O(1) immutable snapshots and O(1) shared-root transaction forks.
- [x] Eliminate full-tree rebuilds from normal in-memory mutation and lookup paths.
- [x] Preserve immutable physical `source_record_index` independently from mutable logical ordinal across reorder and reopen.
- [x] Persist logical move/reorder through generation publication with torn-temp recovery and fail-closed committed-generation validation.
- [x] Reopen committed logical order through `CompactArenaReader`, `LayoutWindowEngine`, and `ZenithHotScrollSession`.
- [x] Complete the repository-level residual audit for logical-ordinal physical-source dereferences.

Validated M2 evidence:

- Source authority head `d17628a3d51922d50fd295f84b436e13e935846a` passed exact-head CI `31795891050`.
- Evidence-only promotion head `959e3b00c7b64b7fa96524905b9f81fe06a70d75` passed required push-triggered exact-head CI `31796822424` and PR CI `31797517189`.
- PR #97 merged the promoted authority as `e132538834b55bb2b40157997b20693201bf6f78`.
- Fresh post-#159 residual audit found no production path that reinterprets mutable logical `record_index` as a physical MassiveDoc source locator. Ordinary layout, checkpoint, hot-scroll, prefetch, export, persisted-height and source-record semantic consumers remain keyed by `source_record_index` where physical identity is required.

M2's promoted persistent mutation contract is move/reorder plus height persistence. Durable arbitrary compact-arena insert/erase are explicit non-capabilities, not incomplete M2 checklist items. If structural editing is required later, it must receive its own durable storage/publication protocol rather than being retroactively claimed by this promotion.

## M3 — Crash-safe segmented generations and mobile I/O

- [x] Add configurable immutable content blocks and bounded `pread`/`ReadFile` / windowed-I/O backends.
- [x] Maintain 32-bit-process-safe windows while preserving 64-bit source/file positions.
- [x] Implement bounded hot/warm/cold admission, promotion, demotion and eviction.
- [x] Add checksummed crash-safe generation manifests and PREPARE/COMMIT append-journal publication.
- [x] Add bounded background compaction, serialized publication boundaries and corruption quarantine.
- [x] Preserve giant records as segmented ranges instead of whole-record materialization.

Validated M3 evidence:

- Frozen source authority `d8d9f11d1bcc1dea12b82d0fa9b2b3f69aa1d9c0` passed exact-head CI `31814283673` SUCCESS.
- Evidence-only promotion head `c64dbf07cefed9d3028b6b6d273412d15db0f1aa` passed required push-triggered CI `31815073323` and PR CI `31816948381`, both SUCCESS.
- PR #99 merged as canonical `4101680d1cb07af67fe280de04187a275e68124a`.
- Exact post-merge main CI `31817594515` completed SUCCESS.

Exit gates:

- [x] Exact 4 GiB corpus opens on the legacy profile without OOM and within the admitted PSS bounds.
- [x] First viewport becomes usable from an immutable progressive prefix before primary import/index completion.
- [x] Resident bounded cold-store file-backed pages are included in measured process PSS.

M3 is canonically complete. It does not manufacture durable arbitrary compact-arena insert/erase; that capability remains outside the admitted persistent mutation contract.

## M4 — Bounded search and full-document operations

- [x] Add bounded block Bloom summaries.
- [x] Store compressed trigram postings on disk with canonical source-identity binding.
- [x] Use scalar-authoritative exact verification with optional x86-64 SSE2 runtime acceleration and scalar fallback everywhere.
- [x] Add bounded Unicode 17 normalization and default full case-fold search with source-byte span preservation.
- [x] Add an O(1) immutable full-document selection descriptor.
- [x] Stream logical-order text and escaped HTML export with fixed memory and transactional target preservation.
- [x] Check cancellation at bounded search/export work boundaries and never promote partial accelerated results to authority.

Validated M4 evidence:

- Frozen source authority `6b3124c28af9b4da84badd88a1be628971df6a0e` passed Windows/Linux CI `31881129599` and dedicated Unicode 17 authority `31881129602`, both SUCCESS.
- Evidence-only promotion head `afb29736ffc283b638e32030374afd09880058ee` passed required push CI `31881601896`, dedicated Unicode authority `31881601910` with no drift, and PR CI `31882003576`, all SUCCESS.
- PR #101 merged as canonical `6b4ed79cbed8a299b94deab5067327258f9e9124`.
- Required post-merge main CI `31883789266` completed SUCCESS and post-merge Unicode authority `31883848777` completed SUCCESS/no drift.
- A later same-SHA run `31884410454` was cancelled and is not admission authority.

M4 is canonically complete. ARM64 NEON source exists but is explicitly not runtime-certified by the current authority matrix.

## M5 — Frame-budget scheduler

Code-side implementation/admission:

- [x] Use deterministic device-profile-specific frame budgets and hard optional-work accounting.
- [x] Use velocity-aware speculative prefetch epochs and stale-work cancellation.
- [x] Schedule visible layout first; optional/background work receives only bounded leftover budget.
- [x] Reject blocking disk/cache/checkpoint/source/height-persistence work on the UI lane rather than silently performing it.
- [x] Use bounded worker-side source prefetch and production hot-scroll consumption.
- [x] Use shared foreground layout handoff/worker execution and process-level asynchronous runtime ownership.
- [x] Shrink/retire cache and runtime state under memory pressure before unbounded growth.
- [x] Provide physical frame certification tooling, exact candidate binding and offline receipt verification.

Canonical implementation evidence:

- PR #113 consolidated the code-side M5 stack at exact head `61db7ae7090302e2135a73811e356dc4d32d2d88` after exact-head CI `32752618029` completed 5/5 SUCCESS.
- PR #113 merged as `3ab7fd5aaf95fa7fa603de0abed88f3d0c3cb924`.
- The natural post-merge main run `32753909790` is retained as FAILURE rather than being rewritten as green: Windows build/headless, both Unicode authorities and Apple guard passed, while the Linux suite failed only `runtime-generation-retirement-tests` with `public session identity could not be reused beside retired generation`.
- The exposed generation snapshot race was repaired on the subsequent canonical line by PR #118, exact-head CI `32851123817` SUCCESS, followed by published-main run `32858032499` SUCCESS.

Remaining physical evidence gate under issue #102:

- [ ] Run the admitted physical candidate wrapper on an eligible physical device/host with exact candidate HEAD and a clean tracked worktree.
- [ ] Perform a fresh candidate-head rebuild and collect native frame-latency samples under the admitted environment/profile contract.
- [ ] Collect and retain physical-device identity and thermal evidence for the same exact run.
- [ ] Verify manifest/evidence equality and SHA-256 binding through the admitted offline receipt verifier.
- [ ] Preserve a certified source-bound physical receipt/evidence bundle.

M5 code-side implementation is complete, but M5 physical frame-latency/thermal credit is **not** complete. Hosted CI, VMs, containers, synthetic receipts and hand-authored PASS files cannot close #102.

## M6 — Cross-platform low-memory backend

- [x] Windows: preserve host-memory fallback, capture `LowMemoryResourceNotification` and immediate-job accounting/limit telemetry, and use the OS low-memory signal only as a conservative pressure floor. Do not infer an effective nested-job memory domain from the immediate job alone.
- [x] Linux: use cgroup v2 effective memory-domain data and PSI where available with procfs fallback and bounded adaptive sampling at 1000 ms Normal / 250 ms Elevated / 100 ms Critical.
- [x] Android: provide the native trim-memory / low-RAM policy/controller contract. Java/Kotlin/JNI callback wiring remains an application-shell integration boundary until such a shell exists in this repository.
- [x] Apple platforms: remain intentionally unsupported under the current target policy; the Apple backend removal guard is authoritative.
- [x] 32-bit: preserve 64-bit file positions while enforcing bounded positional-I/O, mapped-window and record-materialization limits appropriate to the process address space.
- [x] Portability: keep scalar exact matching as correctness authority and use SIMD only as optional runtime-selected acceleration.

Validated final M6 scope evidence:

- Canonical Android parent main `dbed266d98651a833a16df85aa5a877d20793404` passed published-main CI `32960351510` 7/7 SUCCESS.
- Final scope reconciliation PR #125 used exact head `a1b5e810be9d0a6468948a968b0dd43f6297bb5b` and natural PR CI `32961337126` 7/7 SUCCESS.
- PR #125 merged as `8953c711d8ef4de15e46ee3c153702e6c3e165f9`.
- Exact post-merge main CI `32962622941` completed SUCCESS.
- Issue #115 is closed as completed.

M6 is canonically complete. It does not resurrect an Apple backend, does not claim Java/Kotlin/JNI shell integration that is absent from this repository, and does not pretend the immediate Windows job limit proves the effective nested-job memory domain. M7 is the next code-side canonical milestone; M5 physical certification remains a separate evidence boundary.

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
- [x] Admit the physical-host/system-fingerprint/physical-Zevryon/publication-manifest milestone through exact-head run `33125276373` on commit `fcd211776675993a8ce7ad0954f2134b24389143`.
- [x] Admit the follow-on v2 raw-artifact admission-replay and artifact-root-containment candidate through its own exact-head admission PR; merge is conditioned on that PR's single natural Windows/Linux CI run succeeding.
- [ ] Run the real six-runtime readiness preflight on the final physical benchmark host and preserve its M0 machine/thermal evidence artifact.
- [ ] Collect one complete canonical 6x2 browser evidence bundle on that same physical system with no runtime substitution and observed thermal evidence.
- [ ] Collect Zevryon `virtualized` and `native-dom` evidence through `m7_zevryon_physical_case.py`, preserving certified M0 machine/thermal receipts immediately before and after each normalized case.
- [ ] Admit the four artifacts through the v2 collection binder, including preflight/browser physical-host certification, both Zevryon before/after physical receipts, artifact SHA-256 receipts and stable runtime identity checks.
- [ ] Evaluate the fixed five-metric rule on the admitted bundle. Exit status `2` means valid evidence that does not satisfy leadership, not a harness failure.
- [ ] Create the canonical v2 publication manifest only after constraining the admission/raw artifacts to `artifact_root`, re-hashing and re-reading all four raw artifacts, replaying `admit_collection()`, verifying exact clean Git commit/tree, and binding physical-host receipts, runtime identities and evaluator result.
- [ ] Publish the complete admitted evidence and failure modes without cherry-picking metrics across repeat bundles.

One leadership decision consumes one complete evidence bundle. Repeats may be collected as independent reproducibility evidence, but best-of-N reruns and cross-bundle metric mixing are not admissible. Any repeated-run aggregation policy must be frozen before collecting the evidence it would aggregate.

## M8 — 100/100 gate

Final M8 certification remains no-compensation: every required axis must independently pass from raw evidence.

Implementation state:

- [x] Freeze the deterministic storage crash-cut contract without relabeling cut-return tests as real process/power-loss evidence.
- [x] Expose publication cuts after payload flush, PREPARE, durable manifest temp, published manifest and COMMIT while preserving historical cut numeric values.
- [x] Expose a resumable compaction cut after the first durable stale-manifest quarantine while preserving historical compaction cut numeric values.
- [x] Wire `m8-storage-crash-cut-tests` into the normal CTest path to verify pre-COMMIT non-promotion, same-generation retry, post-COMMIT recovery and resumable partial quarantine.
- [x] Add the destructive child-process crash runner over every frozen publication/compaction cut and emit machine-readable fresh-process restart evidence; admitted by PR #134 exact-head run `34107984010`.
- [x] Bind Titan giant-record, unbroken-token and pathological-grapheme dimensions into the `score_100` evaluator authority so none can be omitted or compensated.
- [ ] Collect and admit raw full-Titan evidence proving every content-envelope dimension, including the three adversarial size axes.
- [x] Implement the strict raw four-profile no-compensation evaluator authority with exact profile-set enforcement and recomputed `score_100` results.
- [ ] Collect and admit real raw observations for every device profile against its hard memory cap and frozen latency/throughput limits.
- [x] Implement the continuous dual-mode soak authority with monotonic checkpoint continuity, process-memory receipts and a short CI smoke using the same code path.
- [ ] Execute and admit one continuous certification-mode soak lasting at least 86,400 measured seconds after setup/warmup.
- [x] Implement the deterministic mixed-mutation integrity authority with terminal raw receipts, five-class coverage, snapshot verification and a 50,000-operation CI smoke path.
- [ ] Execute and admit a certification-mode mixed-mutation run with at least 10,000,000 completed operations and deterministic integrity receipts.
- [x] Implement the deterministic Unicode, serializer, index and sequence property-fuzz authority with per-domain receipts and same-seed replay smoke coverage.
- [ ] Execute and admit certification-mode property fuzzing with at least 10,000 completed cases in each of the four domains.
- [x] Build and admit the final M8 evidence binder v2 that recomputes every gate and rejects compensation, hand-authored profile observations and mixed-run cherry-picking; admitted by PR #143 exact-head run `34141947892` on `183eb00a45e92a3a3a4dcd5d7fc6b8eeca60495a`, squash-merged as canonical `ba02e59f47cdbf5314d5eca4c51282aac9a78e68`.
- [ ] Publish final certification only with zero crash/OOM, data corruption, invalid UTF-8 output or logical-order mismatch.
