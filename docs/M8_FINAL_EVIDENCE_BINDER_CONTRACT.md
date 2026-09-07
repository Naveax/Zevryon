# M8 final no-compensation evidence binder v2 contract

## Purpose

Final M8 certification is one immutable decision over one clean candidate commit/tree and one pre-frozen evidence bundle. It is not a collection of unrelated green screenshots, hand-authored pass booleans, best-of-N reruns or successful axes copied from different attempts.

The v2 stack adds provenance-bound import of four independently collected physical profile attempts while preserving the existing raw recomputation authority for storage crash, mixed mutation, continuous soak and four-domain property fuzz.

## Admission boundary

This contract describes the binder-v2 candidate represented by PR #143. It becomes canonical only after the exact-head natural PR CI succeeds and the PR is merged to `main`.

A candidate implementation or a short CI smoke is never final certification evidence.

## Pre-freeze rule

Before any long-run artifact is placed in the final bundle, `scripts/m8_freeze_bundle.py` creates one create-only `bundle-plan.json` under an empty artifact root outside the source repository.

The plan freezes:

- one bundle id;
- exact clean Git commit;
- exact clean Git tree;
- fixed artifact paths;
- fixed receipt paths;
- fixed final-output path;
- SHA-256 values for the explicitly frozen authority source files.

The complete Git tree remains the candidate-content authority. The explicit authority-source hashes are an additional drift receipt, not a replacement for the tree identity.

The bundle root must be absent or empty at freeze time and must be outside the repository.

## Single-use artifact slots

The frozen bundle contains five logical evidence axes:

- `profile`;
- `storage_crash`;
- `mixed_mutation`;
- `soak`;
- `property_fuzz`.

The four executable long-run axes are launched through `scripts/m8_bundle_run.py`. Each has one fixed artifact path and one fixed invocation receipt path. Existing artifacts or receipts are rejected rather than overwritten.

The runner binds:

- bundle id;
- candidate commit/tree;
- exact command argv after placeholder expansion;
- SHA-256 of command arguments that were files at launch;
- monotonic and UTC start/end timestamps;
- return code;
- artifact presence, byte count and SHA-256;
- whether candidate identity remained unchanged;
- whether frozen authority-source hashes remained unchanged.

A failed slot is evidence. It is not deleted and retried inside the same bundle.

## Four-profile physical collection rule

The final profile artifact may not be supplied as a prepared `profile-observations.json`.

Exactly four independently collected physical attempts are required:

1. `legacy-phone`;
2. `mid-phone`;
3. `modern-phone`;
4. `desktop`.

Each attempt is produced by `scripts/m8_profile_case_collector.py` and must bind the same exact candidate commit/tree and the same certification-mode Titan corpus/report.

The collector records create-only evidence including:

- raw profile case;
- physical-host before/after certification receipt;
- aggregate process-group PSS samples;
- production probe phase receipts;
- profile StoreReadConfig application;
- full copy output identity and UTF-8 status;
- Titan report/container identities;
- probe binary identity;
- candidate commit/tree.

Physical identity overrides are rejected. The requested profile must match the certified physical host class.

## Independent provenance verification

Before a profile attempt can enter the final bundle, `scripts/m8_profile_case_provenance_verifier.py` independently reopens and verifies the supplied attempt artifacts and external authority files.

It recomputes or revalidates, among other things:

- case SHA-256;
- provenance SHA-256;
- physical-host receipt SHA-256 and host certification;
- Titan report SHA-256;
- Titan container SHA-256;
- profile probe binary SHA-256;
- copy-output SHA-256;
- candidate commit/tree binding;
- device class and case id;
- aggregate PSS from per-PID samples;
- raw phase-to-case metric binding;
- profile runtime-policy application;
- zero data-loss, invalid-UTF8 and crash/OOM receipts.

The independent verifier therefore performs the external byte re-hash before import.

## Profile bundle import v2

`scripts/m8_bundle_import_profile.py` accepts exactly four `--attempt DEVICE=DIRECTORY` bindings plus the shared Titan report, Titan container and profile probe.

For every attempt it reruns the independent provenance verifier. It then executes the four-profile collection binder v2 and writes the canonical profile observations plus immutable support evidence under `profile-support/`.

The bundle stores small support documents rather than copying the multi-gigabyte Titan container four times. Support evidence includes:

- collection receipt;
- each profile case;
- each profile provenance document;
- each physical-host receipt;
- each independent verification receipt.

The importer receipt additionally binds the SHA-256 set for every support file and the external Titan/report/probe/copy identities verified during import.

If the evidence is structurally invalid, import fails closed. If evidence is valid but a no-compensation profile gate fails, the valid failure is preserved and exit status `2` is used.

## Final profile receipt v2 validation

At final decision time, the binder-v2 wrapper does not trust the profile importer summary alone.

It:

- requires the v2 importer authority/kind;
- re-hashes the canonical profile artifact;
- re-hashes every immutable `profile-support/` file;
- reopens strict UTF-8 JSON support documents;
- checks candidate commit/tree on collection and verification receipts;
- requires exactly the four device classes;
- cross-checks case ids and verification hashes;
- cross-checks case/provenance/physical-host hashes against independent verifier receipts;
- cross-checks Titan report/container, probe and copy-output identities against the external-artifact hashes independently verified at import time;
- requires the recorded four-profile gate result to pass for a final PASS.

The final binder intentionally does not require the original physical-host filesystem paths to remain mounted. That would make an archived bundle non-portable. External bytes are independently rehashed during import, then their identities are sealed into immutable verifier/import receipts whose bytes are rehashed again at final decision time.

## Raw final recomputation rule

The final binder does not accept stored `gate_passed`, `certification_eligible`, `score_100` or equivalent booleans as sufficient authority. It independently recomputes the required raw gates.

### Profile and Titan

The canonical four-profile evaluator is rerun from the sealed raw observations. All four profiles must independently satisfy `score_100` with no compensation.

The Titan envelope includes the adversarial dimensions, including the frozen full logical UTF-8 payload, giant-record, unbroken-token and pathological-grapheme requirements. A stronger desktop result may not compensate for a phone failure.

### Fresh-process storage crash

The final binder validates the complete frozen process-crash matrix:

Publication cuts:

- `after-payload-flush`;
- `after-prepare`;
- `after-manifest-temp`;
- `after-manifest`;
- `after-commit`.

Compaction cuts:

- `after-journal-temp`;
- `after-journal-replace`;
- `after-stale-quarantine`.

It recomputes process-invocation ordering, exit code `86`, recovery generations, source identity, authority payload, segment inventory, retry/resume behavior and quarantine counts.

`power_loss_certified` must remain `false`. Abrupt fresh-process recovery evidence may not be relabeled as physical power-loss evidence.

### Mixed mutation

Certification requires at least 10,000,000 requested and completed operations.

The binder independently requires:

- every requested operation completed;
- all five mutation classes exercised;
- class-count sum equal to completed operations;
- nonzero verification checkpoints;
- equal nonzero live/oracle logical-order digests;
- zero logical-order mismatches;
- zero integrity mismatches;
- no failure reason.

The 50,000-operation CI smoke can never satisfy this gate.

### Continuous soak

Certification requires at least 86,400 measured seconds after setup/warmup.

The binder parses the raw JSONL stream and requires the frozen certification timing semantics, including 60-second checkpoints and one-second memory sampling. It validates continuity, one process identity for the run, query progress in both modes, memory evidence, checkpoint coverage, bounded checkpoint gaps, zero query/memory-snapshot failures and a nonzero rolling digest.

The three-second CI smoke can never satisfy this gate.

### Four property-fuzz domains

Certification requires at least 10,000 completed cases in each exact domain:

1. Unicode;
2. serializer;
3. index;
4. sequence.

Every domain must complete the requested case count with zero failures and a nonzero deterministic digest.

Short CI replay smoke evidence can never satisfy this gate.

## Immutable final decision

`scripts/m8_final_evidence_binder.py --artifact-root <bundle>` produces the final decision once.

The final output is create-only. An existing final decision is rejected and may not be replaced.

A successful decision binds:

- bundle id;
- candidate commit/tree;
- bundle-plan SHA-256;
- each raw artifact path, size and SHA-256;
- each corresponding receipt path and SHA-256;
- independently recomputed gate details;
- `evidence_valid: true`;
- `gate_passed: true`.

## Exit semantics

- `0`: complete evidence is valid and every no-compensation gate passes;
- `2`: evidence is structurally valid but at least one certification gate fails;
- `1`: evidence is invalid, malformed, path-escaped, hash-mismatched, candidate-mismatched or otherwise cannot support a certification decision.

A valid failed benchmark and an invalid evidence package are deliberately different outcomes.

## Certification boundary

A final M8 PASS applies only to the exact candidate commit/tree sealed in that one bundle.

It does not:

- manufacture missing M7 competitor evidence;
- convert process-crash evidence into physical power-loss evidence;
- permit cross-bundle metric cherry-picking;
- make a short CI smoke equivalent to long certification;
- make a hand-authored profile observation document equivalent to four physical collector attempts.