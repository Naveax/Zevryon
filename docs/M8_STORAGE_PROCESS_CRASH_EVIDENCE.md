# M8 fresh-process storage crash evidence

## Authority

The process-level storage crash authority is `m8-fresh-process-storage-crash-cut-recovery-v1` with report schema `zevryon.m8.storage-process-crash.v1`.

It is the destructive-process follow-on to the deterministic cut contract in `M8_STORAGE_CRASH_INJECTION_CONTRACT.md`. The deterministic C++ test proves cut semantics inside one process. This authority additionally proves that the persisted state remains recoverable after the process performing the transaction exits abruptly and a different process opens the store.

## Crash mechanism

`zevryon-m8-storage-crash-probe` reaches one requested frozen cut and immediately calls `std::_Exit(86)` after the storage operation returns at that cut.

Exit code `86` is reserved by this test authority as the injected abrupt-exit receipt. The Python controller must reject a normal exit, a different nonzero exit, signal-derived exit, timeout, or probe failure as an injected-crash success.

`std::_Exit` deliberately bypasses normal stack unwinding and C++ destructor cleanup. The storage layer itself must already have completed whatever durable boundary the selected cut promises before the probe exits.

This is process-termination evidence. It is **not** physical power-loss, torn-sector, volatile-drive-cache-loss, kernel-panic or storage-device-failure certification. Every emitted report therefore carries `power_loss_certified: false`.

## Publication matrix

The controller must execute all five publication cuts independently from fresh fixture roots:

- `after-payload-flush`;
- `after-prepare`;
- `after-manifest-temp`;
- `after-manifest`;
- `after-commit`.

Each case starts with committed generation 1 and attempts generation 2 in the crash child.

Expected fresh-process recovery immediately after the crash is:

- generation 1 for every pre-COMMIT cut;
- generation 2 for `after-commit`.

Every pre-COMMIT case must then complete an exact generation-2 retry and a second fresh recovery must return generation 2 with the exact deterministic source identity, 160-byte authority payload and segment inventory.

For `after-manifest`, the abandoned published final manifest must be preserved exactly once as `.uncommitted` quarantine evidence before retry. Other publication cuts must not manufacture that quarantine class.

## Compaction matrix

The controller must execute all three compaction cuts independently after seeding four committed generations and retaining the newest two:

- `after-journal-temp`;
- `after-journal-replace`;
- `after-stale-quarantine`.

Fresh recovery after every abrupt exit must retain generation 4 with the exact authority identity. A normal resume must then finish compaction without changing that authority.

Before resume, stale-quarantine file count must be:

- zero after `after-journal-temp`;
- zero after `after-journal-replace`;
- exactly one after `after-stale-quarantine`.

After resume there must be exactly two stale-manifest quarantine receipts.

## Raw report requirements

A passing machine-readable report must contain:

- exact schema and authority;
- SHA-256 of the crash probe executable used;
- injected crash exit code;
- the complete ordered publication and compaction cut lists;
- one terminal result for every required cut;
- recovery object after every crash;
- retry/resume recovery object where required;
- quarantine receipt counts;
- `fresh_process_recovery: true`;
- `power_loss_certified: false`;
- top-level `gate_passed: true` only when every case independently passes.

Recovery validation must check the committed generation, deterministic source-identity hash material, exact authority payload size/content sentinel and exact segment inventory. Merely opening the store without an exception is not sufficient.

## Failure semantics

Any missing cut, unexpected exit code, timeout, invalid recovery JSON, wrong generation, identity drift, authority payload drift, segment inventory drift, retry failure, resume failure or quarantine-count mismatch makes the report fail closed.

A failed raw report must be preserved as failure evidence rather than rerunning the identical binary/input until it happens to pass. A code fix creates a new exact candidate and a new evidence run.

## Final M8 boundary

Passing this process-crash authority satisfies only the process-termination crash axis implementation. Final M8 certification still requires the Titan envelope, all device profiles, 24-hour soak, ten-million mixed mutations, four property-fuzzing domains and the final no-compensation evidence binder. Physical power-loss testing may be added as a stronger independent authority later and must not be inferred from this report.
