# M8 final no-compensation evidence binder v2 contract

## Purpose

M8 certification is one immutable decision over one exact clean candidate and one pre-frozen evidence bundle. Passing results from unrelated runs may not be combined. Summary booleans are never authority when the underlying evidence can be recomputed.

The v2 profile path closes the remaining provenance gap: a prepared `profile-observations.json` is not accepted as certification evidence. The bundle imports four independently collected physical profile attempts, independently verifies each attempt, recomputes the four-profile collection, and preserves the resulting support receipts for final revalidation.

## Frozen candidate and bundle

`m8_freeze_bundle.py` creates one create-only `bundle-plan.json` under a fresh artifact root outside the repository. The plan freezes:

- one bundle id;
- exact clean Git commit and tree;
- fixed artifact paths for profile, storage-crash, mixed-mutation, soak and property-fuzz evidence;
- fixed receipt paths and final decision path;
- SHA-256 receipts for explicitly listed authority sources.

The exact Git tree remains the complete tracked-source authority. If the candidate commit/tree or frozen authority sources change after freeze, the bundle is invalid.

## Single-use slots

Every artifact/receipt slot is create-only. A failed run seals its slot. Do not delete a failure and retry the same axis in the same bundle. A correction or reproducibility repeat requires a new bundle root.

The executable axes run through `m8_bundle_run.py`. It records exact argv, command-file hashes where applicable, monotonic/UTC process boundaries, return code, artifact SHA-256/size, and candidate/source stability.

## Physical-profile import v2

`m8_bundle_import_profile.py` requires exactly four `DEVICE=ATTEMPT_DIRECTORY` bindings for:

- `legacy-phone`;
- `mid-phone`;
- `modern-phone`;
- `desktop`.

Each attempt must be the create-only output of the admitted physical profile collector and must contain the expected case, provenance, physical-host and copy-output evidence.

The importer also requires one canonical certification-mode Titan report, its Titan container, and the exact profile probe executable.

For each profile the importer reruns `m8_profile_case_provenance_verifier.py`. The verifier binds:

- candidate commit/tree;
- device class and case id;
- case/provenance/physical-host SHA-256 receipts;
- canonical Titan report/container identity;
- full-copy SHA-256;
- probe binary SHA-256;
- before/after physical-host recertification;
- aggregate process-group PSS evidence;
- raw phase/sample boundaries and profile StoreReadConfig application.

Only after all four independent verification receipts pass does `m8_profile_collection_binder_v2.py` recompute the exact four-profile `profile-observations.json` and collection receipt.

A hand-authored observations document cannot enter this v2 path.

## Profile support evidence

The importer stores the small immutable evidence needed for later replay under `profile-support/` inside the frozen artifact root:

- four raw case documents;
- four provenance documents;
- four physical-host receipts;
- four independent verification receipts;
- the four-profile collection receipt.

The large Titan container and per-profile full-copy outputs are not duplicated into the bundle. Their exact SHA-256 identities are recorded as external artifact bindings. This avoids multiplying the 4+ GiB Titan payload while retaining tamper detection.

The profile import receipt binds every support path/hash plus Titan report/container, profile probe and copy-output hashes. Candidate/tree and authority-source stability are checked again before the import is sealed.

## Final profile-receipt revalidation

`m8_final_evidence_binder.py` does not trust the profile import receipt merely because it says `evidence_valid: true`.

Before invoking the raw no-compensation recomputation implementation, the wrapper:

1. reloads the frozen plan with current candidate/source verification;
2. re-hashes every immutable profile-support file;
3. rechecks all four verifier receipt identities;
4. cross-checks case ids, candidate commit/tree and device classes;
5. cross-checks case/provenance/physical-host/Titan/probe/copy SHA-256 bindings;
6. verifies the collection support receipt and observations hash;
7. verifies the external Titan report/container and profile probe still match the import receipt.

Any missing support file, path escape, overwrite, hash drift or identity mismatch is evidence-invalid.

## Raw no-compensation recomputation

The underlying raw binder implementation remains independently testable and recomputes every M8 axis.

### Profile/Titan

The canonical evaluator must independently pass all four device profiles. It also enforces the Titan envelope, including 4 GiB logical UTF-8 payload, record/node/style/resource dimensions, 64 MiB largest record, 16 MiB unbroken token and 64 KiB pathological grapheme. No desktop surplus compensates for a phone miss.

### Fresh-process storage crash

All five publication cuts and all three compaction cuts are replayed from raw process receipts. Exit code, process ordering, generation authority, quarantine behavior, retry/resume state and deterministic recovery content are recomputed. `power_loss_certified` must remain false.

### Mixed mutation

Certification requires at least 10,000,000 requested and completed operations, all five mutation classes, matching live/oracle order digests, nonzero verification checkpoints and zero integrity/order mismatches.

### Continuous soak

Certification requires at least 86,400 measured seconds after setup/warmup, frozen checkpoint/memory intervals, one continuous process identity, bounded checkpoint gaps, progress in both modes, nonzero rolling digest and no query/memory failures.

### Property fuzz

Certification requires at least 10,000 completed cases in each exact domain: Unicode, serializer, index and sequence. Every domain must complete with zero failures and a nonzero deterministic digest.

## Final decision

`final-certification.json` is create-only.

Exit semantics:

- `0`: evidence is structurally valid and every recomputed gate passes;
- `2`: evidence is structurally valid but at least one real certification gate fails;
- `1`: evidence is malformed, incomplete, path-escaped, candidate-mismatched, hash-mismatched, tampered or otherwise invalid.

Valid gate failure and invalid evidence are deliberately distinct.

## Certification boundary

Passing this binder certifies only M8 for the exact frozen candidate represented by the bundle. It does not manufacture missing M7 competitor evidence, does not turn abrupt process termination into physical power-loss evidence, and does not allow CI smoke runs to substitute for the real 10M/24h/10k-per-domain certification runs.