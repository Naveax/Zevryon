# M8 four-profile observation contract

## Authority

Raw profile input schema: `zevryon.m8.profile-observations.v1`.

Recomputed gate output schema: `zevryon.m8.profile-gate.v1` with authority `m8-four-profile-no-compensation-v1`.

This gate exists so final M8 certification cannot treat one device profile as representative of another or trust hand-authored verdicts.

## Exact top-level schema

A raw input document contains exactly these top-level fields:

- `schema`;
- `candidate_commit`;
- `candidate_tree`;
- `observations`.

Missing fields or any extra top-level field make the evidence invalid. In particular, collector-authored `gate_passed`, `score_100`, `checks` or equivalent verdict fields are forbidden rather than ignored.

## Exact profile set

One input artifact must contain exactly one raw observation for each frozen device class:

- `legacy-phone`;
- `mid-phone`;
- `modern-phone`;
- `desktop`.

Missing profiles, duplicates or unknown profile names make the evidence invalid. They are not interpreted as zero, skipped or inherited from another profile.

## Raw observation rule

Each observation contains only the fields required to reconstruct `BenchmarkObservation`. Derived fields such as `score_100`, `checks`, `passed` or other precomputed verdicts are forbidden in the raw observation object.

The authority reconstructs `BenchmarkObservation` and calls the repository's canonical `performance_contract.evaluate()` itself.

The raw fields include all Titan content dimensions, including:

- total UTF-8 bytes;
- record, logical-node, style-run and resource-reference counts;
- largest record bytes;
- largest unbroken token bytes;
- pathological grapheme bytes;

and all frozen profile performance/correctness fields:

- process-group PSS;
- preindexed and streaming first viewport latency;
- scroll P99;
- maximum normal stall;
- warm and cold exact-search latency;
- mutation P95;
- copy throughput;
- data-loss event count;
- invalid-UTF8 event count;
- crash/OOM count.

All numeric values must be finite and non-negative. Integer-count fields must be JSON integers rather than floats disguised as counts.

## Candidate binding

The raw document requires lowercase 40-hex `candidate_commit` and `candidate_tree` identifiers. The profile gate carries them into its output unchanged and hashes the complete raw input bytes with SHA-256.

This slice validates the binding format and preserves the identifiers. Final artifact-root admission must additionally verify that those identifiers equal the clean Git candidate being certified rather than trusting text supplied by a collector.

## No-compensation rule

Every one of the four observations is evaluated independently.

Top-level `gate_passed` in the recomputed output is true only if all four recomputed per-profile `score_100` values are true. A desktop surplus cannot compensate for a legacy-phone miss. Passing a hard cap does not compensate for missing the stricter target when `score_100` requires both.

The Titan adversarial dimensions are also recomputed independently through the canonical evaluator. One byte below any required Titan threshold fails that profile's score.

## Exit semantics

The command-line authority uses three exit classes:

- `0`: evidence is structurally valid and all four profiles recompute to `score_100`;
- `2`: evidence is structurally valid but at least one profile fails one or more required gates;
- `1`: evidence itself is invalid, malformed or incomplete.

A valid nonpassing measurement is evidence, not a harness crash. It must be preserved rather than silently rerun until it passes.

## Final evidence boundary

This authority evaluates raw observations; it does not manufacture the observations. Final M8 still requires a collector that records real measurements under the frozen profile conditions and binds them to the exact candidate/environment.

Passing this gate also cannot compensate for missing 24-hour soak, >=10M mutation, process-crash, fuzzing or final binder evidence.
