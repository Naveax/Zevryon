# M8 four-domain property-fuzz contract

## Authority

Machine-readable report schema: `zevryon.m8.property-fuzz.v1`.

Authority: `m8-four-domain-property-fuzz-v1`.

The authority exercises four production correctness domains independently:

- `unicode`;
- `serializer`;
- `index`;
- `sequence`.

One top-level PASS requires every domain to complete the exact requested case count with zero failures. No domain can compensate for another.

## Deterministic case identity

Every run records one root seed. Each domain/case receives a deterministic derived seed. A failing domain records:

- zero-based failing case index;
- exact derived failing seed;
- failure reason;
- number of cases completed before failure;
- deterministic domain digest accumulated through completed checks.

Replaying the same candidate, root seed and case count must reproduce the same passing domain digests or the same first failing case/seed unless the implementation changed.

## Unicode properties

The Unicode domain uses the production `UnicodeSearchNormalizer` with the generated production normalization tables.

For generated valid Unicode scalar streams it verifies:

- one-shot normalization equals randomly chunked streaming normalization, including source spans;
- every emitted value remains a valid Unicode scalar;
- emitted source spans are ordered within the original source extent;
- normalizing already-normalized output preserves normalized values;
- deterministic output digest receipts are stable under same-seed replay.

This complements the full Unicode conformance authority; it does not replace the normative Unicode data suite.

## Serializer properties

The serializer domain exercises the production MassiveDoc store plus compact-arena persistence path.

For generated stores/configurations it verifies:

- generated store finalization succeeds;
- compact arena build statistics match the generated configuration;
- serialized arena header/configuration roundtrips through a fresh `CompactArenaReader`;
- logical IDs, source lengths and source-record identities roundtrip for every generated record;
- a persisted height mutation survives closing and reopening the reader;
- reopened aggregate height and mutated record height match the pre-close receipt.

The oracle is generated independently from the values supplied to the writer rather than by re-parsing the arena serializer output.

## Index properties

The index domain builds a real MassiveDoc store with randomized segmentation/search-block configuration and randomized record payloads.

For generated present and absent byte queries it compares `StoreReader.find()` against an independent brute-force `std::string::find()` oracle over the original source records.

The exact ordered hit set must agree on:

- record index;
- logical ID;
- byte offset.

Any false negative, false positive or offset/identity disagreement fails the domain.

## Sequence properties

The sequence domain exercises the production `ChunkedOrderStatisticsSequence` against an independent `std::vector<SequenceRecord>` oracle.

Generated mutation sequences cover insert, erase, move, height update and search-summary update. During and after mutation it verifies:

- aggregate record count, text bytes, layout height and search-summary OR;
- exact rank/order and record identity;
- text and height prefix offsets;
- sampled `prefix()` aggregates;
- `locate_text_offset()` selection;
- `locate_height_offset()` selection;
- old-value/erased-record receipts;
- immutability of a pre-mutation copy-on-write snapshot.

## Smoke versus certification

The normal Windows/Linux CTest path runs a short deterministic smoke. The smoke exists to validate the implementation and replay semantics; it is not final fuzz certification evidence.

Final fuzz certification requires **at least 10,000 completed cases per domain in one admitted run**.

`--certification` with fewer than `10,000` requested cases per domain is invalid configuration and must fail before a report can claim certification eligibility.

A smoke report must therefore retain:

- `mode: "smoke"`;
- `certification_minimum_cases_per_domain: 10000`;
- `certification_threshold_met: false` when below that threshold;
- `certification_eligible: false`;
- four separate domain receipts.

## Failure and exit semantics

- exit `0`: structurally valid run and every requested case in every domain passed;
- exit `2`: a production property failed; the terminal machine-readable report is preserved;
- exit `1`: invalid configuration, work-directory/report I/O failure, or invalid production Unicode tables prevented a valid fuzz run.

A domain failure is evidence. Do not rerun the identical candidate/seed merely to obtain a preferred result; fix the candidate and create new evidence.

## Final M8 boundary

This contract implements the four-domain fuzz evidence authority and deterministic CI smoke/replay path. It does not by itself certify M8.

Final M8 still requires admitted certification-mode fuzz evidence, raw Titan evidence, four-profile evidence, the continuous 24-hour soak, the >=10,000,000 mixed-mutation run, storage-crash evidence and the final no-compensation binder.
