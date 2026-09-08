# M3-M6 Canonical Status Reconciliation

## Purpose

This document reconciles the current mainline execution plan with already-admitted M3, M4, M5 and M6 implementation history. It adds no runtime capability and does not convert hosted CI into physical-device evidence.

Historical promotion receipts remain evidence snapshots of the point at which they were written. Their old `awaiting-*` status text is not rewritten retroactively; this reconciliation records what happened afterward.

## M3 — canonical storage hardening closure

M3 is canonically complete.

Authority chain:

- frozen source authority: `d8d9f11d1bcc1dea12b82d0fa9b2b3f69aa1d9c0`;
- source exact-head CI: `31814283673` — SUCCESS;
- evidence-only promotion head: `c64dbf07cefed9d3028b6b6d273412d15db0f1aa`;
- required push-triggered promotion-head CI: `31815073323` — SUCCESS;
- PR exact-head CI: `31816948381` — SUCCESS;
- promotion PR: #99;
- canonical merge: `4101680d1cb07af67fe280de04187a275e68124a`;
- canonical post-merge main CI: `31817594515` — SUCCESS.

The admitted scope includes crash-safe segmented generation publication and recovery, bounded positional I/O beyond 4 GiB, bounded hot/warm/cold residency, generation compaction/publication, progressive immutable preview publication and all three M3 exit gates: legacy-profile 4 GiB open without OOM, first viewport before primary import completion, and resident cold-store pages included in measured process PSS.

The M3 promotion does not create durable arbitrary compact-arena insert/erase. That remains outside the admitted persistent mutation contract.

## M4 — canonical bounded-search closure

M4 is canonically complete.

Authority chain:

- frozen source authority: `6b3124c28af9b4da84badd88a1be628971df6a0e`;
- source exact-head Windows/Linux CI: `31881129599` — SUCCESS;
- source dedicated Unicode 17 authority: `31881129602` — SUCCESS;
- evidence-only promotion head: `afb29736ffc283b638e32030374afd09880058ee`;
- required push-triggered promotion-head Windows/Linux CI: `31881601896` — SUCCESS;
- promotion-head dedicated Unicode authority: `31881601910` — SUCCESS/no drift;
- PR exact-head CI: `31882003576` — SUCCESS;
- promotion PR: #101;
- canonical merge: `6b4ed79cbed8a299b94deab5067327258f9e9124`;
- canonical post-merge main CI: `31883789266` — SUCCESS;
- post-merge dedicated Unicode authority: `31883848777` — SUCCESS/no drift.

A later same-SHA Windows/Linux run, `31884410454`, was cancelled and is not used as admission authority. M4 closure rests on the earlier successful required post-merge run above, not on cherry-picking a convenient later run.

The admitted M4 scope includes bounded Bloom/trigram candidate acceleration, source-identity binding, exact fail-closed fallback, bounded cancellation, Unicode 17 normalization/default full case-fold search, O(1) full-document selection, fixed-memory transactional text/escaped-HTML export and scalar-authoritative exact matching with x86-64 SSE2 runtime acceleration.

ARM64 NEON source exists but remains explicitly not runtime-certified by the current authority matrix.

## M5 — code-side complete, physical gate still open

M5 code-side implementation is complete and present on canonical main. Physical-device frame/thermal certification remains deliberately open under issue #102.

Implementation admission:

- consolidation PR: #113;
- exact consolidation head: `61db7ae7090302e2135a73811e356dc4d32d2d88`;
- exact-head CI: `32752618029` — 5/5 SUCCESS;
- consolidation merge: `3ab7fd5aaf95fa7fa603de0abed88f3d0c3cb924`.

Historical post-merge nuance:

- the natural main run `32753909790` on `3ab7fd5a...` was FAILURE;
- Windows build/headless, both Unicode authorities and Apple removal guard succeeded;
- Linux build succeeded, but its headless suite failed only `runtime-generation-retirement-tests` with `public session identity could not be reused beside retired generation`;
- the failure was retained rather than rerun until green;
- the generation snapshot race was repaired in the subsequent canonical M6 line, with PR #118 exact-head CI `32851123817` SUCCESS and published-main run `32858032499` SUCCESS.

Current M5 code-side scope includes deterministic per-profile frame budgets, visible-first scheduling, bounded optional work, UI blocking-I/O rejection, velocity-aware speculative prefetch/cancellation, bounded worker-side prefetch, hot-scroll consumption, asynchronous ownership/runtime services, pressure-driven retirement and the complete exact-candidate-bound physical certification/receipt-verification tooling.

M5 is **not** physically certified yet. Hosted CI, VMs, containers, synthetic receipts or hand-authored PASS files cannot close #102. Closure requires a real eligible physical execution producing a valid exact-candidate-bound native frame-latency and thermal receipt.

## M6 — canonical cross-platform low-memory closure

M6 is canonically complete and issue #115 is closed as completed.

The admitted line includes:

- Linux cgroup v2 effective-domain accounting, PSI capture and conservative pressure integration with procfs fallback and bounded adaptive sampling;
- Windows low-memory notification context and a conservative low-memory pressure floor while keeping immediate-job limit data as telemetry rather than falsely treating it as an effective nested-job memory domain;
- bounded 32-bit address-space/file-position behavior with 64-bit source positions;
- scalar-authoritative exact matching with optional runtime-selected SIMD acceleration;
- a native Android trim-memory/low-RAM policy/controller contract, while Java/Kotlin/JNI shell callback wiring remains outside this repository until such a shell exists;
- Apple platforms intentionally unsupported under the current target policy, protected by the Apple backend removal guard.

Final scope reconciliation authority:

- canonical Android parent main: `dbed266d98651a833a16df85aa5a877d20793404`;
- parent published-main run: `32960351510` — 7/7 SUCCESS;
- final scope PR: #125;
- exact scope head: `a1b5e810be9d0a6468948a968b0dd43f6297bb5b`;
- exact-head natural PR CI: `32961337126` — 7/7 SUCCESS;
- canonical scope merge: `8953c711d8ef4de15e46ee3c153702e6c3e165f9`;
- canonical post-merge main CI: `32962622941` — SUCCESS.

M6 does not resurrect an Apple backend and does not claim a trustworthy effective nested Windows job limit from the immediate job alone.

## Current queue boundary

After M3/M4 canonical closure, M5 code-side completion and M6 canonical closure, the remaining milestone blockers are evidence boundaries rather than missing M3-M6 engine implementation:

- M5 / #102: real physical frame-latency and thermal receipt;
- M7 / #126: one complete real physical six-runtime competitor bundle and leadership evaluation/publication;
- M8 / #133: full Titan, four physical profile attempts, crash matrix, >=10,000,000 mixed mutations, >=10,000 property-fuzz cases per domain, >=86,400-second continuous dual-mode soak, and one frozen no-compensation final bundle.

Those boundaries must remain fail-closed. Documentation or hosted CI cannot manufacture their missing physical/long-running evidence.
