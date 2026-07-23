# Z1D-B3 — Bounded Unicode 17 bracket and neutral resolution

Z1D-B3 resolves Unicode 17 UAX #9 revision 51 rules N0, N1, and N2 for
each isolating run sequence produced by Z1D-B1 after W1-W7 processing from
Z1D-B2.

## Implemented scope

- deterministic generation of the Unicode 17.0.0 `BidiBrackets.txt` table;
- exact verification of all 128 paired-bracket records across the full Unicode
  code-point range;
- BD16 bracket-pair discovery with a fixed stack of exactly 63 entries;
- canonical equivalence between U+3009 and U+232A during closing-bracket
  matching;
- logical-order sorting and sequential N0 processing of bracket pairs;
- dynamic preceding-strong context that observes bracket pairs resolved earlier
  by N0;
- EN and AN treatment as strong R within N0 and N1/N2 boundary searches;
- propagation of an N0-resolved bracket direction to immediately following
  characters whose original bidi type was NSM;
- N1 resolution when the surrounding strong directions agree;
- N2 fallback to embedding-level parity when the boundaries disagree;
- one-byte resolved output per X9-active scalar;
- immutable explicit units, weak types, source code points, and sequence
  topology;
- publish-on-success output with no partial result after validation or
  allocation failure;
- a dedicated `BidiNeutralResolution` Resource Ledger budget.

## Sequential N0 requirement

Bracket pairs are processed in logical order of their opening positions. A
pair resolved earlier in that order immediately changes the current bidi types
seen by later pairs. In particular, a resolved outer opening bracket can become
the preceding strong context for a nested pair. The implementation advances a
single context cursor through each isolating run sequence, so this normative
behavior remains O(n) without repeated backward scans.

## Memory contract

The certification workload uses a 64 KiB hard cap for N0-N2 output and all
temporary state.

- output: one `BidiClass` byte per active scalar;
- bracket matching: fixed 63-entry stack on the native stack;
- pair records: at most 16 bytes each and scoped to one isolating run sequence;
- topology validation: one temporary visited byte per level run;
- no duplicate source-text, explicit-unit, weak-type, or sequence-member array;
- no second full-size endpoint-direction buffer.

BD16 stack overflow is not an allocation failure. The affected isolating run
sequence publishes an empty bracket-pair list for N0 and continues through
N1/N2, as required by the bounded pairing algorithm.

## Fail-closed validation

The resolver rejects:

- active streams that do not match the weak-type output size;
- code-point or unit indices outside their backing arrays;
- units marked as removed by X9;
- weak-stage classes that should already have been eliminated by W1-W7;
- non-contiguous level runs;
- duplicated, missing, misordered, or level-inconsistent sequence links;
- any allocation that exceeds the `BidiNeutralResolution` hard cap.

On failure, the public output vector is empty and no partial N0-N2 result is
published.

## Certification gates

The final workflow requires:

- Unicode version: 17.0.0;
- UAX #9 revision: 51;
- pinned `BidiBrackets.txt` SHA-256:
  `dadbaf38a0d0246e5b805bf8725cb81b7c621f93d030595635f5ba2c2f179428`;
- generated-table fingerprint:
  `429b90895269f7307e4aa63f05827996296ad3369d1af444f94cc12938cbce43`;
- 1,114,112 code-point property scan;
- 128/128 bracket records, including 64 open and 64 close records;
- adversarial sequential-N0, NSM, N1/N2, overflow, topology, stage-order, and
  allocation tests;
- three independent 64 KiB benchmark distributions;
- median P95 at or below 1.50 ms;
- median P99 at or below 2.00 ms;
- worst observed sample at or below 4.00 ms;
- peak neutral-resolution memory at or below 65,536 bytes;
- zero rejected reservations and zero accounting errors;
- strict GCC, AppleClang, and MSVC builds;
- Linux ASan/UBSan and dedicated MSVC AddressSanitizer coverage;
- existing ZENITH, UTF-8, grapheme, script, explicit-bidi, sequence, and weak
  type regressions.

## Explicit boundary

Z1D-B3 does **not** implement:

- I1-I2 implicit embedding-level adjustment;
- L1 whitespace and separator level reset;
- L2 visual reordering;
- L3 combining-mark handling for reordering;
- L4 mirroring;
- glyph-facing visual order.

Z2 shaping must not consume Z1D-B3 output as final reordered text. Those steps
belong to Z1D-C.
