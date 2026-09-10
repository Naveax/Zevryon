# Z7 html5lib tokenizer full-corpus census v1

## Purpose

This slice establishes an honest execution census across the complete tokenizer provenance v2 denominator. It is a diagnostic baseline and regression authority, not a conformance-gate closure.

The pinned authority is:

- upstream repository: `html5lib/html5lib-tests`
- upstream commit: `224991ec10db04f056a89eed8b0bd8695fd2950e`
- fixture files: **14**
- test objects: **6810**
- initial-state executions: **7036**

The first admitted census run is GitHub Actions run `34477049718`.

## Frozen baseline

The exact v1 census result is:

- **5660 passed** by exact token stream, parse-error stream and common-stat comparison;
- **129 failed** after the production tokenizer completed but disagreed with the external authority;
- **1247 unsupported** because the production boundary or probe deliberately fails closed for that surface;
- total accounting: **5660 + 129 + 1247 = 7036**.

`config/z7_html5lib_tokenizer_full_corpus_census_v1.json` is an immutable observed snapshot. It freezes the totals, every per-file partition and every reason bucket. Production improvements do not rewrite that snapshot in the same change merely to keep CI green.

## Two verification policies

`scripts/z7_html5lib_tokenizer_full_corpus_census_v1_baseline.py` deliberately exposes two policies.

### `exact`

`exact` is the authority-snapshot policy. It requires the live report to equal the admitted v1 snapshot exactly, including total counts, all 14 per-file partitions and every reason bucket. This is used when validating or deliberately promoting the census authority itself.

### `no-regression`

`no-regression` is the production-change gate. It keeps the denominator fixed at 14 files / 6810 tests / 7036 executions but permits genuine implementation improvements before a later baseline promotion.

A production report is accepted only when all of the following hold:

- global `passed` does not decrease;
- global `failed` does not increase;
- global `unsupported` does not increase;
- for every individual fixture, `passed` does not decrease, `failed` does not increase and `unsupported` does not increase;
- every existing failure/unsupported reason bucket can only stay equal or decrease;
- a new failure/unsupported reason bucket cannot appear with a positive count;
- `passed:exact-match` can only stay equal or increase;
- total, per-file and reason accounting remain internally exact.

The per-file and per-reason checks are intentional. Aggregate-only comparison would allow a regression in one fixture or recovery class to be hidden by an unrelated improvement elsewhere, which is precisely the sort of accounting trick this authority exists to prevent.

A successful production improvement therefore lands independently. A later authority-only change may then update the frozen exact snapshot after the new distribution has been reviewed and reproduced.

The dedicated pull-request workflow uses `--policy no-regression`. The verifier self-test exercises both policies, including positive improvement, passed-to-unsupported regression, failed-to-unsupported reclassification, aggregate-neutral cross-file regression and aggregate-neutral reason relabeling.

## Strong existing surfaces

Several substantial external fixtures already execute with no failure or unsupported cases at this boundary:

- `namedEntities.test`: **4210 / 4210 passed**;
- `numericEntities.test`: **336 / 336 passed**;
- `entities.test`: **80 / 80 passed**;
- `contentModelFlags.test`: **24 / 24 passed**;
- `test1.test`: **69 / 69 passed**;
- `pendingSpecChanges.test`: **1 / 1 passed**.

This is useful implementation evidence, but it is not promoted into a full-corpus conformance claim while other fixtures remain incomplete.

## Failure surface

The 129 completed-but-mismatching executions are split into:

- **120 parse-error-stream mismatches**;
- **9 token-stream mismatches**.

By fixture:

- `unicodeChars.test`: **94 failures**;
- `test3.test`: **34 failures**;
- `domjs.test`: **1 failure**.

All other pinned fixtures currently have zero completed-but-mismatching executions. Unsupported executions remain separate and are not counted as failures or passes.

## Main unsupported debt

The largest unsupported production boundaries are:

- malformed DOCTYPE-name recovery: **384**;
- PUBLIC/SYSTEM or malformed DOCTYPE recovery: **353**;
- RCDATA/RAWTEXT executions requiring a non-empty last-start-tag: **102**;
- attribute-name byte surface outside the current subset: **77**;
- raw NUL input preprocessing: **67**;
- CDATA initial state: **56**;
- first attribute without separating whitespace recovery: **50**;
- bogus-comment recovery: **33**;
- CR input preprocessing: **32**;
- bogus-comment end-tag recovery: **22**;
- missing DOCTYPE-name recovery: **16**;
- nullable DOCTYPE-name probe-wire limitation: **14**.

Smaller buckets remain explicitly recorded in the machine baseline rather than disappearing into a generic unsupported count.

## Census classification rules

The runner distinguishes three outcomes:

1. `passed`: the production probe completes and token stream, parse-error stream and common counters exactly match the pinned expected execution;
2. `failed`: the production probe completes but its externally observable result disagrees, times out, emits malformed wire output or otherwise produces an execution error;
3. `unsupported`: the harness identifies a deliberately unrepresented authority surface before execution, or the production tokenizer returns its explicit fail-closed status.

The runner applies html5lib's documented `doubleEscaped` `\\uHHHH` preprocessing to test input/output structures. It does not use that mechanism to imitate HTML input-stream preprocessing inside the tokenizer.

Explicit pre-execution unsupported classes include CDATA initial state, raw NUL and CR preprocessing, non-UTF-8-scalar test data, nullable DOCTYPE names that the current probe wire cannot distinguish, and the historical `xmlViolation.test` infoset-coercion convention.

## Current priority

The census points to DOCTYPE recovery as the highest-leverage production slice: the two largest fail-closed buckets account for **737 executions** before including the separate 16 missing-name cases. Recovery work should be admitted in bounded production slices with focused regressions and the full-corpus `no-regression` gate before any new exact census snapshot is promoted.

The 129 actual mismatches are a separate correctness queue. In particular, the `unicodeChars.test` parse-error behavior should be investigated without conflating it with deliberately unsupported recovery.

## Nonclaims

This census does not claim that unsupported executions pass, does not hide failed executions, and does not satisfy `html_tokenizer_conformance`.

It also does not satisfy `tree_builder_conformance`, `chunk_size_equivalence`, `parser_fuzzing`, `bounded_large_document_parse`, or Z7 completion. `html_tokenizer_conformance_claim` and `z7_status_change` remain false until the relevant canonical gates are actually closed.
