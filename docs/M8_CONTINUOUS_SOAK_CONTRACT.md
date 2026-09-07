# M8 continuous soak contract

## Authority

Event schema: `zevryon.m8.soak-event.v1`.

Authority: `m8-continuous-dual-mode-soak-v1`.

This authority proves that one long-lived Zevryon process can keep both persistent MassiveDoc query modes alive continuously while producing monotonic continuity, query-integrity and process-memory receipts. It does not treat a collection of short independent launches as a continuous soak.

## Process and session continuity

One probe process builds one deterministic synthetic store and opens two persistent `MassiveDocBenchmarkSession` instances against it:

- `virtualized`;
- `native-dom`.

After setup and warmup, the measured soak timer begins. The same process alternates deterministic queries through both already-open sessions until the requested duration is reached or a terminal failure occurs.

Every emitted event carries the process ID. A certification artifact may not splice events from different process IDs into one apparent soak.

## Duration rule

Final M8 soak certification requires at least **86,400 measured seconds** after setup and warmup have completed.

`--certification` with a requested duration below `86400` seconds is invalid configuration and is rejected before a soak evidence stream starts.

The normal Windows/Linux CTest path runs a short smoke with the same probe and verification logic. A smoke completion event must carry `certification_eligible:false`. A smoke PASS proves the harness works; it is not 24-hour evidence.

## Continuous checkpoint rule

Certification mode emits continuity checkpoints on a frozen 60-second cadence. Smoke mode uses a one-second cadence so the same mechanism can be exercised in CI.

Checkpoint events carry:

- monotonic elapsed milliseconds since measured soak start;
- checkpoint ordinal;
- actual gap since the preceding checkpoint;
- maximum observed checkpoint gap;
- cumulative virtualized and native query counts;
- rolling deterministic query-receipt digest;
- current and peak process RSS;
- cumulative memory-snapshot count;
- maximum observed query duration per mode.

Checkpoints are never backfilled after a delay. One actual checkpoint is emitted when the probe regains control, and the next due time is scheduled from that real observation. This prevents a paused or stalled process from manufacturing multiple historical checkpoints at one timestamp.

An actual checkpoint gap greater than twice the frozen interval fails closed. The terminal event also recomputes the maximum-gap gate from the raw observed state.

## Query integrity receipts

The rolling digest consumes deterministic query receipt fields rather than wall-clock query timing:

- mode;
- query coordinate;
- source bytes read;
- rendered height;
- checkpoint source offset;
- fragment count;
- truncation flag.

Each virtualized query must return nonzero source bytes and rendered height. Each native query must return nonzero fragments and rendered height. Negative query timing or a query API failure terminates the soak as failure.

Both modes must complete at least one measured query. A run exercising only one mode cannot pass.

## Memory evidence

The probe calls the production `capture_zenith_process_memory_snapshot()` authority throughout the soak.

Smoke mode samples frequently enough to exercise the path in CI. Certification mode uses a one-second memory-sample interval. The event stream records current RSS, peak RSS, sample count and memory-snapshot failures.

A passing terminal gate requires at least one successful memory sample and zero memory-snapshot failures. These receipts document soak memory behavior; final device-profile certification still evaluates profile-specific memory targets separately.

## Event stream

A started soak produces line-delimited JSON events:

1. one `start` event after store creation, both session opens and warmup succeed;
2. zero or more actual `checkpoint` events while the measured run is active;
3. one terminal `complete` event.

Setup failures that occur after the event writer is available are preserved as `setup-failure` events rather than being presented as a started soak.

The `start` event binds the deterministic store payload size and SHA-256, requested duration, checkpoint cadence, memory cadence and setup duration.

The `complete` event recomputes and records:

- measured elapsed time;
- duration target gate;
- checkpoint coverage gate;
- checkpoint-gap gate;
- query totals for both modes;
- final rolling digest;
- process-memory receipts;
- query and memory failure counts;
- certification eligibility;
- failure reason when applicable;
- terminal gate result.

## Failure semantics

Query failure, empty/invalid query receipt, memory-snapshot failure, output failure, excessive continuity gap, insufficient measured duration, insufficient checkpoint coverage, missing dual-mode activity or missing rolling digest fails closed.

A valid failing soak is evidence. It must be preserved rather than rerunning the same candidate/environment until a pass appears.

Invalid command configuration is not a started soak and exits before emitting misleading certification evidence.

## Final M8 boundary

This slice implements and smoke-tests the continuous soak evidence authority. It does **not** certify 24 hours merely because the short CI smoke succeeds.

Final M8 still requires one admitted certification-mode run satisfying the full 86,400-second measured duration, plus the independent Titan, four-device-profile, >=10M mixed-mutation, storage-crash, fuzzing and final binder authorities. No axis compensates for another.
