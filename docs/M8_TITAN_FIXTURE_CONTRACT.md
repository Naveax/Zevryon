# M8 canonical Titan fixture contract

## Authority

Schema: `zevryon.m8.titan-fixture.v1`.

Authority: `m8-canonical-titan-fixture-v1`.

The Titan fixture is the canonical raw MassiveDoc corpus used to prove the frozen M8 content envelope. The authority writes the normal `ZMDOC001` container, binds the exact clean Git commit/tree, records deterministic whole-container and logical-payload SHA-256 receipts, and reopens the special adversarial records before reporting PASS.

A report is not a performance result. It proves the corpus that later profile measurements consume. Latency, process-group PSS, search, mutation and copy throughput remain separate measured gates.

## Frozen certification envelope

Certification mode requires exactly:

- 4 GiB logical UTF-8 payload (`4,294,967,296` bytes);
- 8,388,608 logical records;
- 67,108,864 logical nodes;
- 33,554,432 style runs;
- 1,048,576 resource references;
- one 64 MiB largest record;
- one 16 MiB unbroken token;
- one 64 KiB pathological grapheme sequence.

Certification configuration is not tunable. Supplying an override that differs from the frozen values is invalid configuration and fails before any corpus or report is created.

## Independent adversarial records

The first three records are reserved so the three size axes cannot accidentally collapse into one synthetic value.

### Record 0: 64 MiB giant record

Record 0 is exactly 64 MiB and consists of the repeating ASCII pattern `g `.

The embedded spaces are deliberate. This record proves the giant-record axis without also becoming a giant token.

### Record 1: 16 MiB unbroken token

Record 1 is exactly 16 MiB of ASCII `T` bytes and contains no whitespace or delimiter bytes. The re-open verifier checks every byte in the record and recomputes its SHA-256.

### Record 2: 64 KiB pathological grapheme

Record 2 is exactly 65,536 UTF-8 bytes:

- one `U+00E9` base code point (2 UTF-8 bytes);
- 32,767 `U+0301` combining acute marks (65,534 UTF-8 bytes).

Under Unicode extended-grapheme rules the combining marks extend the preceding base, so this construction is one deliberately pathological grapheme sequence. The authority reopens the record and requires exact byte equality with this construction.

## Remaining corpus

All later records use the existing deterministic `generate_massivedoc_corpus.py` adversarial payload patterns, including mixed ASCII/code, Turkish normalization text, Arabic/Hebrew bidi text, CJK and the existing synthetic payload pattern. The remaining byte budget is distributed deterministically while preserving at least one byte per record.

The final ordinary record contains the frozen `ZEVRYON_M8_TITAN_TAIL` marker.

## Raw receipts

The report records:

- candidate commit and tree;
- observed and frozen envelope values;
- physical file byte count;
- whole-container SHA-256;
- logical-payload SHA-256;
- exact record-header and payload offsets for the three special records;
- exact special-record sizes and SHA-256 values;
- re-open verification state for the ZMDOC header, physical size and each special payload.

The expected physical file size is recomputed as:

`HEADER.size + logical_records * RECORD_HEADER.size + logical_utf8_bytes`.

A mismatch fails closed.

## Smoke versus certification

Normal CTest executes a much smaller fixture through the same generator/re-open code path. The smoke freezes its own deterministic dimensions only so CI can exercise the authority without writing a multi-gigabyte artifact.

A smoke report must retain:

- `mode: "smoke"`;
- `certification_threshold_met: false`;
- `certification_eligible: false`.

The tests also build the smoke twice and require identical container, payload and special-record hashes.

## Create-only behavior

The corpus and report paths are create-only. Existing output is never overwritten. An invalid certification configuration creates neither file.

A real certification run should use an artifact directory outside the repository. The source repository must be clean before generation so the report can bind one exact candidate commit/tree.

## Exit semantics

- `0`: the requested smoke or exact certification fixture was generated and its raw special records revalidated;
- `2`: a structurally generated fixture failed its post-write verification gate;
- `1`: invalid configuration, dirty candidate, Git failure or I/O/format failure prevented valid evidence.

## M8 boundary

This authority closes the missing canonical Titan-corpus construction boundary. It does not by itself complete M8 certification.

After admission, the next required authority is a raw four-device profile measurement collector that consumes this exact Titan fixture and measures the frozen memory, viewport, scroll, stall, search, mutation and copy-throughput fields. Final M8 still also requires the real 24-hour soak, >=10,000,000 mixed mutations, >=10,000 property-fuzz cases per domain, fresh-process crash evidence and the no-compensation final evidence binder.
