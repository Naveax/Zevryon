# M8 fresh-process storage crash evidence

## Authority

The process-level storage crash authority is `m8-fresh-process-storage-crash-cut-recovery-v1` with report schema `zevryon.m8.storage-process-crash.v1`.

It is the destructive-process follow-on to the deterministic cut contract in `M8_STORAGE_CRASH_INJECTION_CONTRACT.md`. The deterministic C++ test proves cut semantics inside one process. This authority additionally proves that the persisted state remains recoverable after the process performing the transaction exits abruptly and a fresh process opens the store.

## Crash mechanism

`zevryon-m8-storage-crash-probe` reaches one requested frozen cut and immediately calls `std::_Exit(86)` after the storage operation returns at that cut.

Exit code `86` is reserved by this authority as the injected abrupt-exit receipt. The controller rejects a normal exit, a different nonzero exit, signal-derived exit, timeout or probe failure as an injected-crash success.

`std::_Exit` deliberately bypasses normal stack unwinding and C++ destructor cleanup. The storage layer itself must already have completed whatever durable boundary the selected cut promises before the probe exits.

This is process-termination evidence. It is **not** physical power-loss, torn-sector, volatile-drive-cache-loss, kernel-panic or storage-device-failure certification. Every emitted report therefore carries `power_loss_certified: false`.

## Fresh-process receipt rule

Every seed, crash, retry/resume and recovery operation is launched by a separate `subprocess.Popen` invocation. The controller records for each invocation:

- monotonically assigned invocation id;
- operating-system PID;
- monotonic start timestamp;
- monotonic end timestamp;
- terminal return code.

Fresh recovery is proven by a distinct controller invocation whose start timestamp is not earlier than the predecessor's recorded end timestamp. PID is preserved as raw OS evidence but **numeric PID inequality is not required**, because an operating system is allowed to recycle a terminated process identifier. Treating PID reuse as proof of process reuse would turn a correctness gate into a flaky lottery.

A case without valid invocation/lifetime receipts cannot claim fresh-process recovery merely by setting a summary boolean.

These receipts prove separate process creation and ordering for this harness. They do not elevate process termination into physical power-loss evidence.

## Publication matrix

The controller executes all five publication cuts independently from fresh fixture roots:

- `after-payload-flush`;
- `after-prepare`;
- `after-manifest-temp`;
- `after-manifest`;
- `after-commit`.

Each case starts with committed generation 1 and attempts generation 2 in the crash child.

Expected fresh-process recovery immediately after the crash is generation 1 for every pre-COMMIT cut and generation 2 for `after-commit`.

Every pre-COMMIT case then completes an exact generation-2 retry and a second fresh recovery must return generation 2 with the exact deterministic source identity, 160-byte authority payload and segment inventory.

For `after-manifest`, the abandoned published final manifest must be preserved exactly once as `.uncommitted` quarantine evidence before retry. Other publication cuts must not manufacture that quarantine class.

## Compaction matrix

The controller executes all three compaction cuts independently after seeding four committed generations and retaining the newest two:

- `after-journal-temp`;
- `after-journal-replace`;
- `after-stale-quarantine`.

Fresh recovery after every abrupt exit must retain generation 4 with the exact authority identity. A normal resume must then finish compaction without changing that authority.

Before resume, stale-quarantine file count must be zero after `after-journal-temp`, zero after `after-journal-replace`, and exactly one after `after-stale-quarantine`. After resume there must be exactly two stale-manifest quarantine receipts.

## Raw report requirements

A passing machine-readable report contains:

- exact schema and authority;
- SHA-256 of the crash probe executable used;
- injected crash exit code;
- complete ordered publication and compaction cut lists;
- one terminal result for every required cut;
- raw invocation/PID/start/end/return-code receipts for seed, crash, recovery and retry/resume processes;
- a verified fresh-process receipt for every individual case;
- recovery object after every crash;
- retry/resume recovery object where required;
- quarantine receipt counts;
- `process_receipt_semantics: "separate-popen-invocation-with-nonoverlapping-monotonic-lifetime-v1"`;
- `fresh_process_recovery: true` and `fresh_process_receipts_verified: true` only after every per-case process receipt passes;
- `power_loss_certified: false`;
- top-level `gate_passed: true` only when every case independently passes.

Recovery validation checks the committed generation, deterministic source-identity hash material, exact authority payload size/content sentinel and exact segment inventory. Merely opening the store without an exception is insufficient.

## Failure semantics

Any missing cut, unexpected exit code, timeout, missing or invalid process receipt, impossible process-lifetime ordering, invalid recovery JSON, wrong generation, identity drift, authority payload drift, segment inventory drift, retry failure, resume failure or quarantine-count mismatch fails closed.

A failed raw report is preserved as failure evidence rather than rerunning the identical binary/input until it happens to pass. A code fix creates a new exact candidate and a new evidence run.

## Final M8 boundary

Passing this process-crash authority satisfies only the process-termination crash axis implementation. Final M8 certification still requires the Titan envelope, all device profiles, 24-hour soak, ten-million mixed mutations, four property-fuzzing domains and the final no-compensation evidence binder. Physical power-loss testing may be added as a stronger independent authority later and must not be inferred from this report.
