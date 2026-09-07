# M8 final no-compensation evidence binder contract

## Purpose

Final M8 certification is one immutable evidence decision over one pre-frozen candidate and one pre-frozen bundle. It is not a collection of unrelated green screenshots, hand-authored pass booleans or best-of-N cherry-picked runs.

Final output schema: `zevryon.m8.final-certification.v1`.

Final authority: `m8-no-compensation-raw-evidence-binder-v1`.

## Pre-freeze rule

Before any long certification run, `m8_freeze_bundle.py` creates `bundle-plan.json` under an empty artifact root outside the repository.

The plan freezes:

- one random 128-bit bundle id;
- exact clean Git commit;
- exact clean Git tree;
- exact artifact and receipt paths;
- exact final-output path;
- SHA-256 of the explicitly frozen authority entrypoints, probes and orchestration sources listed by `AUTHORITY_SOURCE_FILES`.

The exact Git tree is the complete candidate-content authority and therefore binds every tracked implementation file, including supporting/internal modules that are not duplicated in the explicit authority-source hash list. The explicit source hashes are an additional drift receipt for the files that define or launch certification authorities; they do not replace the full-tree binding.

The artifact root must not be inside the source repository. Evidence generation therefore cannot make the candidate tree dirty merely by writing results.

The plan is create-only. Existing non-empty roots are rejected and the plan is never overwritten.

## Single-use artifact rule

The frozen bundle has exactly five raw artifact slots:

- `profile-observations.json`;
- `storage-process-crash.json`;
- `mixed-mutation.json`;
- `continuous-soak.jsonl`;
- `property-fuzz.json`.

Each slot has one fixed receipt path. The four executable authorities run through `m8_bundle_run.py`. A slot whose artifact or receipt already exists is sealed and cannot be rerun in the same bundle.

The runner records:

- bundle id and exact candidate commit/tree;
- artifact key and exact frozen path;
- invocation start/end UTC and monotonic timestamps;
- exact argv;
- SHA-256 of command arguments that were files at launch;
- process return code;
- artifact presence, size and SHA-256;
- whether clean Git identity remained equal to the frozen candidate;
- whether all frozen authority-source hashes remained unchanged.

A failed authority run is not erased and replaced by another attempt in the same bundle. A fix or independent repeat requires a different bundle.

## Physical profile import rule

The four-device raw observation document is imported exactly once through `m8_bundle_import_profile.py`.

Before import the canonical `m8_profile_observation_gate.evaluate_document()` authority is rerun from raw observations. The document must bind the same exact candidate commit and tree frozen in the bundle plan.

Invalid profile evidence seals a failure receipt. Valid but nonpassing profile evidence remains valid failure evidence and returns exit status `2`.

A hand-authored profile JSON is not certification evidence merely because it satisfies the schema. The raw observation document must come from an admitted measurement collector whose own receipts bind every measured field to the frozen candidate and measurement environment. Until that collector is admitted and real observations are collected, the physical-profile axis remains open.

## Final recomputation rule

`m8_final_evidence_binder.py` reopens and re-hashes the plan, all five artifacts and all five receipts.

The binder does **not** trust stored `gate_passed`, `certification_eligible`, `score_100` or equivalent summary booleans as certification authority. Raw receipts are recomputed first. Summary booleans are accepted only when they agree with the recomputed result.

### Profile and Titan

The binder reruns the canonical four-profile evaluator from raw observations. This independently requires all four device classes to reach `score_100` and therefore also recomputes every Titan content-envelope gate, including:

- 4 GiB logical UTF-8 payload;
- record/node/style/resource dimensions;
- 64 MiB largest record;
- 16 MiB unbroken token;
- 64 KiB pathological grapheme.

No stronger desktop result compensates for a phone-profile miss.

### Fresh-process storage crash

The binder independently validates the complete frozen five-publication/three-compaction cut matrix.

It recomputes:

- injected exit code `86`;
- unique process invocation receipts;
- monotonic predecessor/successor ordering;
- expected committed generation after every cut;
- deterministic source identity;
- exact 160-byte authority payload receipt;
- exact segment inventory;
- exact retry/resume generation;
- `.uncommitted` quarantine behavior;
- stale-manifest quarantine counts.

`power_loss_certified` must remain `false`. Process termination evidence may not be relabeled as physical power-loss evidence.

### Mixed mutations

The binder requires certification mode and independently checks:

- at least 10,000,000 requested and completed operations;
- completion of every requested operation;
- all five mutation classes exercised;
- exact per-class count sum;
- nonzero verification checkpoints;
- equal nonzero live/oracle logical-order digests;
- zero logical-order and integrity mismatches;
- no failure reason.

A 50,000-operation CI smoke can never satisfy this gate.

### Continuous soak

The binder parses the JSONL stream rather than trusting the terminal summary.

It requires:

- certification mode;
- at least 86,400 measured seconds after setup/warmup;
- the frozen 60-second certification checkpoint interval;
- the frozen one-second memory sample interval;
- one process id for the complete run;
- strictly increasing checkpoint elapsed times;
- checkpoint gaps derived exactly from neighboring raw timestamps;
- no gap exceeding twice the frozen interval;
- sufficient checkpoint coverage;
- progress in both virtualized and native-dom modes;
- memory evidence;
- zero memory-snapshot/query failures;
- a nonzero rolling digest;
- no terminal failure reason.

A three-second CI smoke can never satisfy this gate.

### Four property-fuzz domains

The binder requires certification mode and at least 10,000 completed cases in each exact domain:

1. Unicode;
2. serializer;
3. index;
4. sequence.

Every domain must complete the exact requested case count with zero failures, no failure receipt and a nonzero deterministic digest.

The short same-seed CI replay smoke can never satisfy this gate.

## Immutable final decision

`final-certification.json` is create-only. Once a bundle has a final decision, rerunning the binder cannot replace it.

A passing output binds:

- bundle id;
- exact candidate commit/tree;
- bundle-plan SHA-256;
- each raw artifact path, byte count and SHA-256;
- each invocation/import receipt path and SHA-256;
- independently recomputed gate details;
- `evidence_valid: true`;
- `gate_passed: true`.

## Exit semantics

- `0`: the complete frozen evidence bundle is structurally valid and every recomputed gate passes;
- `2`: the bundle is structurally valid but at least one real certification gate fails;
- `1`: the bundle/evidence is invalid, malformed, path-escaped, candidate-mismatched, hash-mismatched or otherwise cannot support a certification decision.

A structurally valid failure and an invalid evidence package are deliberately different outcomes.

## Certification boundary

Passing the binder certifies only the M8 contract for the exact frozen candidate represented by that bundle. It does not manufacture missing physical M7 competitor evidence, does not convert fresh-process crash evidence into physical power-loss evidence, and does not turn a hand-authored profile observation document into measured device evidence.