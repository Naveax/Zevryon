# M8 binder v2 admission checkpoint

## Purpose

This checkpoint records the exact admission state of the final M8 no-compensation evidence-binder v2 slice without treating an unadmitted candidate as canonical authority.

## Canonical parent

- canonical `main`: `e8383ca1b2aaafd43d9367997c95792dd507eb5f`
- canonical parent push CI: `34137280070` — completed SUCCESS, 7/7 jobs
- admitted physical profile collector/provenance verifier: PR #142

## Binder v2 admission PR

- PR: #143 — `M8: admit provenance-bound final evidence binder v2`
- branch: `candidate/m8-final-evidence-binder-v2-prep`
- current exact head: `183eb00a45e92a3a3a4dcd5d7fc6b8eeca60495a`
- current exact tree: `c2e8669707e318311e00197186663947d676382f`
- current natural PR run: `34141947892`

The current head is not canonical until its single natural exact-head PR run succeeds and the PR is merged.

## Retained first failure

The original exact head `3aa2e4bb604211064820baf7943baab850486bd0` received one natural PR run, `34140503331`.

Windows compilation succeeded. The headless suite exposed two test/harness defects:

1. the profile-receipt v2 fixture used an unresolved temporary artifact root while the production binder captures a resolved artifact root;
2. the raw-recomputation harness imported the preserved legacy test corpus without placing repository root on `sys.path`, so `zevryon_platform` was not importable.

That SHA is retained as failed evidence and must not be rerun unchanged.

Current descendant `183eb00a45e92a3a3a4dcd5d7fc6b8eeca60495a` changes only the two affected test/harness files.

## Duplicate-run discipline

- Do not rerun or dispatch another workflow for `3aa2e4bb...`.
- Do not create another equivalent run for `183eb00a...` while `34141947892` is pending, queued or in progress.
- The workflow uses one concurrency group per PR ref with `cancel-in-progress: false`; a newer run may remain pending until the older run releases the group.
- A failing code SHA is replaced by a fixed descendant. It is never retried until green by repetition.

## What binder v2 must admit

The slice is intended to admit all of the following as one coherent authority stack:

- four-profile collection binder v2;
- create-only bundle importer from four physical collector attempts;
- immutable profile-support artifacts under the frozen evidence root;
- independent provenance-verifier receipt binding for case, candidate, Titan, physical host, probe and copy identities;
- final profile receipt v2 revalidation;
- raw no-compensation recomputation for process-crash, mixed-mutation, continuous-soak and four-domain property-fuzz evidence;
- CTest authority covering collection binder v2, profile importer v2, final profile receipt v2 and final raw recomputation.

A prepared hand-authored `profile-observations.json` is not accepted as a substitute for four admitted physical attempts.

## Merge condition

PR #143 may be admitted only after its current exact-head natural run succeeds. If a product or harness defect is found, preserve the failed SHA/run, create one fixed descendant and use only the descendant's natural PR run.

## Post-admission execution order

After binder v2 is admitted on canonical `main`, stop adding certification framework merely to postpone real evidence collection. The remaining M8 work is physical/long-running evidence:

1. pin one clean canonical candidate commit/tree;
2. create the canonical certification-mode Titan corpus and report, including full 4 GiB logical UTF-8 envelope and giant-record/token/grapheme dimensions;
3. collect exactly one physical attempt for each `legacy-phone`, `mid-phone`, `modern-phone` and `desktop` profile using the admitted collector and independent provenance verifier;
4. freeze one fresh external evidence bundle and import those four attempts through binder v2;
5. run the complete fresh-process storage crash matrix into its create-only bundle slot;
6. execute at least 10,000,000 mixed mutations in certification mode;
7. execute at least 10,000 cases in each exact property-fuzz domain: Unicode, serializer, index and sequence;
8. execute one continuous certification-mode soak for at least 86,400 measured seconds after setup/warmup;
9. run the final no-compensation binder exactly once for that bundle;
10. preserve the complete PASS or FAIL bundle. Never replace one failing axis with evidence from another bundle.

Exit status `0` is the only final PASS. Exit status `2` is valid evidence with at least one failed certification gate. Exit status `1` means the evidence package itself is invalid.

## Still-open physical evidence

This checkpoint does not claim any of the following have already been completed:

- real canonical 4 GiB Titan certification evidence;
- all four real physical profile attempts;
- >=10,000,000 mixed-mutation certification evidence;
- >=10,000 property cases per domain;
- >=86,400-second continuous soak;
- one final frozen M8 PASS bundle.

M7 physical competitor evidence remains independent and cannot be manufactured by an M8 PASS.