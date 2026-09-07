# M8 binder v2 admission checkpoint

## Status

The M8 final no-compensation evidence-binder v2 machinery is now admitted to canonical `main`.

- admission PR: #143 — `M8: admit provenance-bound final evidence binder v2`
- exact admitted candidate head: `183eb00a45e92a3a3a4dcd5d7fc6b8eeca60495a`
- exact admitted candidate tree: `c2e8669707e318311e00197186663947d676382f`
- single successful exact-head natural PR run: `34141947892` — 7/7 SUCCESS
- canonical squash-merge commit: `ba02e59f47cdbf5314d5eca4c51282aac9a78e68`
- canonical tree after squash: `c2e8669707e318311e00197186663947d676382f`
- natural post-merge push run: `34158512860`

The squash merge preserves the admitted tree exactly while giving canonical `main` its own commit identity.

## Retained first failure

The original candidate head `3aa2e4bb604211064820baf7943baab850486bd0` received exactly one natural PR run, `34140503331`, and failed.

Windows compilation succeeded but headless testing exposed two narrow test/harness defects:

1. the profile-receipt v2 fixture used an unresolved temporary artifact root while production captures a resolved artifact root;
2. the raw-recomputation harness imported the preserved legacy test corpus without first placing repository root on `sys.path`, so `zevryon_platform` was not importable.

Linux independently confirmed the second defect. On that failed SHA the profile-receipt v2 test passed on Linux while the raw-recomputation test failed with the same missing-module error. No third Linux-specific binder defect was exposed.

The failed SHA was never rerun. Descendant `183eb00a45e92a3a3a4dcd5d7fc6b8eeca60495a` changed only the two affected test/harness files and received one natural PR run, `34141947892`, which completed 7/7 SUCCESS.

## Admitted authority stack

Canonical `main` now contains the admitted machinery for:

- provenance-aware four-profile collection binder v2;
- create-only bundle import from exactly four physical collector attempts;
- immutable profile-support artifacts under one frozen evidence root;
- independent provenance-verifier receipt binding for case, candidate, Titan, physical host, probe and copy identities;
- final profile-receipt v2 revalidation;
- raw no-compensation final recomputation for fresh-process storage crash, mixed mutation, continuous soak and four-domain property fuzz;
- CTest coverage for collection binder v2, profile importer v2, final profile receipt v2 and final raw recomputation.

A prepared or hand-authored `profile-observations.json` is not an admissible replacement for four independently verified physical attempts.

## Duplicate-run discipline retained

- `3aa2e4bb...` / run `34140503331` remains retained failed evidence and must not be rerun unchanged.
- `183eb00a...` / run `34141947892` is the single successful exact-head PR admission run.
- `ba02e59f...` / run `34158512860` is the natural canonical post-merge push run; do not dispatch an equivalent run while it is queued or in progress.
- A failing code SHA is fixed by a descendant. It is not repeatedly rerun until green.

## Remaining M8 work is real evidence

Binder-framework admission is complete. Do not add more certification framework merely to postpone physical or long-running evidence collection.

The remaining execution order is:

1. pin one clean canonical candidate commit/tree;
2. create the canonical certification-mode Titan corpus/report, including the full 4 GiB logical UTF-8 envelope and giant-record/token/grapheme dimensions;
3. collect exactly one physical attempt for each `legacy-phone`, `mid-phone`, `modern-phone` and `desktop` profile using the admitted collector and independent provenance verifier;
4. freeze one fresh external evidence bundle and import all four physical attempts through binder v2;
5. execute the complete fresh-process storage crash matrix into its create-only bundle slot;
6. execute at least 10,000,000 mixed mutations in certification mode;
7. execute at least 10,000 cases in each exact property-fuzz domain: Unicode, serializer, index and sequence;
8. execute one continuous certification-mode soak for at least 86,400 measured seconds after setup/warmup;
9. run the final no-compensation binder exactly once for that bundle;
10. preserve the complete PASS or FAIL bundle without cross-bundle substitution.

Exit status `0` is the only final M8 PASS. Exit status `2` means structurally valid evidence with at least one failed certification gate. Exit status `1` means the evidence package itself is invalid.

## Still-open evidence

Admission of the machinery does not claim completion of:

- real canonical 4 GiB Titan certification evidence;
- all four real physical profile attempts;
- >=10,000,000 mixed-mutation certification evidence;
- >=10,000 property-fuzz cases per domain;
- >=86,400-second continuous soak;
- one final frozen M8 PASS bundle.

M7 physical competitor evidence remains independent and cannot be manufactured by an M8 PASS.
