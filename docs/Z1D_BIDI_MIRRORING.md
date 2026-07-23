# Z1D-C2B — Unicode 17 L4 Mirroring

## Status

- C2B-A: Unicode data generator and conformance — **in progress**
- C2B-B: bounded runtime mirror requests — **not implemented**

Z1D remains incomplete until both parts are merged.

## Unicode contract

UAX #9 rule L4 requires a mirrored glyph when both conditions hold:

1. the character has the normative `Bidi_Mirrored=Y` property;
2. the resolved directionality is `R`, represented by an odd final line level.

Two Unicode properties must remain distinct:

- `Bidi_Mirrored` is normative and determines whether mirroring is required;
- `Bidi_Mirroring_Glyph` is informative and may provide a character whose glyph is an exact or best-fit mirrored form.

An absent character mapping never means `Bidi_Mirrored=N`. Some mirrored characters require font-level alternate glyph selection and have no appropriate mirror codepoint.

## C2B-A data model

Inputs:

- Unicode 17.0.0 `UnicodeData.txt`, field 9;
- Unicode 17.0.0 `BidiMirroring.txt`.

Generated outputs:

- compact sorted ranges for all `Bidi_Mirrored=Y` codepoints;
- sorted informative mirror-codepoint records;
- explicit `best_fit` flag;
- Unicode source SHA-256 values;
- normalized table fingerprint;
- machine-readable generation report.

The generator is deterministic: the same two source files must produce byte-identical header and report files across repeated runs and supported platforms.

## C2B-B runtime model

The runtime stage will consume:

- canonical decoded codepoints;
- X9-active index mapping;
- C2A `visual_to_active` order;
- C2A L1-adjusted final levels.

It will emit a sparse list only for visual positions requiring L4:

```text
odd final level
AND Bidi_Mirrored=Y
→ mirror request
```

Proposed record:

```text
visual position       32 bits
active index          32 bits
source codepoint      32 bits
mapped codepoint      32 bits
flags                  8 bits
```

The final physical representation may be reduced after measurement. The API must preserve these semantic states:

- exact character mapping available;
- best-fit character mapping available;
- mirrored glyph required but no character mapping exists.

The third state is handed to the shaping/font layer. Zevryon must not silently substitute the original codepoint, an unrelated bracket pair, or a guessed codepoint.

## Required correctness tests

- U+0028/U+0029 mirror only at odd final levels;
- U+221B requires mirroring even when no character mapping exists;
- U+FD3E and U+FD3F remain non-mirrored legacy exceptions;
- exact and best-fit records remain distinguishable;
- even-level mirrored characters produce no request;
- visual positions follow C2A `visual_to_active`, not logical order;
- malformed permutations and level vectors fail closed;
- hard-budget failure publishes no partial request list;
- every generated mapping source has `Bidi_Mirrored=Y`;
- generator output is byte-identical on Ubuntu, macOS, and Windows.

## Planned performance gates

Initial 64 KiB goals:

- sparse request record: `<= 16 bytes` target, `<= 20 bytes` hard ceiling;
- no allocation when the visible line contains no odd-level mirrored scalar;
- P95 `<= 0.20 ms` for the standalone L4 request pass;
- P99 `<= 0.30 ms`;
- no source/codepoint stream copy;
- zero rejected reservations in the certification workload;
- zero accounting errors.

Exact gates will be tightened after the first diagnostic artifact.

## Explicit boundary

C2B does not perform:

- OpenType `rtlm` or font-specific alternate selection;
- glyph shaping;
- font fallback;
- text substitution in the canonical source;
- CSS writing-mode handling;
- vertical text mirroring;
- paint or compositor integration.

Those belong to Z2 and later rendering milestones.
