# Z1D-C2A — Bounded L1-L3 Visual Order

## Status

Implementation milestone: **Z1D-C2A**

This stage consumes the immutable X9-active stream and I1-I2 implicit levels. It applies Unicode 17.0.0 UAX #9 revision 51 rules L1-L3 to caller-supplied line partitions.

It is **not** the final Bidi presentation stage. L4 mirroring remains in Z1D-C2B. Shaping, glyph selection, caret mapping, accessibility projection, and rasterization must not treat C2A output as a completed text-rendering result.

## Input contract

- `BidiExplicitUnit` remains immutable.
- `BidiSequenceTopology::active_unit_indices` is strictly increasing and contains no X9-removed unit.
- I1-I2 provides exactly one level per X9-active scalar.
- Every implicit level is within `0..126` and is not below its explicit level.
- The caller supplies `BidiLineSpan` entries after line breaking.
- Line spans form one exact, non-empty, contiguous partition of the active stream.
- Empty input must have no line spans.

The visual stage intentionally does not repeat full BD13 isolating-run topology validation. L1-L3 consumes only the active stream, original classes, implicit levels, and line partitions. Revalidating unused sequence links in this hot path would duplicate the sequence/implicit stage contracts and add avoidable work.

## Output model

C2A publishes two vectors atomically:

- one-byte L1-adjusted level per active scalar;
- one 32-bit `visual_to_active` index per active scalar.

The persistent output cost is therefore exactly:

```text
1 byte level + 4 byte visual index = 5 bytes / active scalar
```

No inverse map, source copy, explicit-unit copy, or resolved-type copy is retained. Logical-to-visual maps needed by selection or accessibility are derived per visible line later.

## Rules

### L1

For each supplied line:

- `B` and `S` reset to paragraph level;
- preceding contiguous `WS`, `FSI`, `LRI`, `RLI`, and `PDI` reset;
- trailing contiguous `WS`, `FSI`, `LRI`, `RLI`, and `PDI` reset;
- lines are processed independently.

### L2

Each line is reordered independently. Contiguous sequences are reversed from the maximum resolved level down to the lowest odd level. Reordering never crosses a caller-supplied line boundary.

The implementation supports the complete resolved-level range through level **126**.

### L3

After L2, combining marks whose original class is `NSM` are repaired so that a base character remains before its following combining sequence in visual order.

## Failure behavior

- Invalid active indices, original classes, levels, or line partitions are rejected.
- Resource exhaustion is reported as `OutputBudgetExceeded`.
- Output is cleared before processing.
- Working vectors are swapped into the public result only after all validation and reordering succeeds.
- No partial result is published after failure.

## Resource contract

Resource class: `BidiVisualOrder`

Final 64 KiB certification budget:

```text
184,320 bytes / 180 KiB
```

Measured persistent output:

```text
35,746 active scalars × 5 bytes = 178,730 bytes
```

The remaining 5,590 bytes are deliberate allocator and certification margin. A separate level-126 stress test verifies that four active scalars use exactly 20 output bytes and add no hidden temporary allocation.

## Unicode 17 conformance

Pinned source:

```text
BidiTest.txt SHA-256
888bdfc8090652272d1f859cdb00ae659e2dc6c26740be61ef1d03998a687620
```

The selected normative subset excludes rows requiring explicit controls, isolates, BN retention, bracket resolution, or L4. Every accepted initial Bidi_Class stream runs through the real pipeline:

```text
W1-W7 → N0-N2 → I1-I2 → L1-L2
```

Certified counts:

- total data lines: **490,846**;
- selected lines: **34,183**;
- paragraph-mode cases: **52,782**;
- automatic-LTR cases: **17,594**;
- forced-LTR cases: **17,594**;
- forced-RTL cases: **17,594**;
- failures: **0**.

L3 is covered by dedicated adversarial tests because `BidiTest.txt` does not certify L3 behavior.

## 64 KiB certification baseline

Diagnostic baseline before final hard-cap tightening:

- input codepoints: **40,512**;
- X9-active scalars: **35,746**;
- lines: **1,192**;
- L1 separator resets: **1,191**;
- L1 whitespace resets: **1,191**;
- L2 reversal spans: **4,766**;
- L2 reordered units: **29,790**;
- L3 combining sequences: **1,192**;
- L3 repaired units: **2,384**;
- median P50: **0.201603 ms**;
- median P95: **0.260302 ms**;
- median P99: **0.273746 ms**;
- worst maximum: **0.343456 ms**;
- peak output memory: **178,730 bytes**.

Permanent certification gates:

- median P95 `<= 0.40 ms`;
- median P99 `<= 0.50 ms`;
- worst maximum `<= 1.00 ms`;
- peak visual memory `<= 180 KiB`;
- rejected reservations `= 0`;
- accounting errors `= 0`.

## Explicit boundary

C2A does not implement:

- L4 mirrored glyph selection;
- `BidiMirroringGlyph.txt` mapping;
- font shaping or glyph substitution;
- line breaking itself;
- logical-to-visual inverse maps;
- cursor, selection, or accessibility hit testing;
- vertical writing modes;
- paint or compositor integration.

Those responsibilities remain in C2B and later Z2-Z6 milestones.
