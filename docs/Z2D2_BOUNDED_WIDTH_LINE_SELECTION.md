# Z2D-2 Bounded Width Line Selection

Z2D-2 converts the legal opportunities produced by Z2D-1 into concrete logical line boundaries using retained HarfBuzz advances and one fixed available inline width.

## Inputs

The selector consumes four already-certified structures over one identical grapheme-cluster domain:

- `MultiRunShapedText` for segment-local glyph advances;
- `GlyphClusterMap` for logical cluster ownership of glyph groups;
- `CaretBoundaryMap` for glyph-safe and HarfBuzz-safe boundaries;
- `LineBreakOpportunityMap` for Unicode `Prohibited`, `Allowed`, and `Mandatory` opportunities.

Horizontal left-to-right and right-to-left shaping runs are supported. Vertical shaping directions are rejected explicitly rather than interpreted with horizontal metrics.

## Selection policy

Selection is greedy and deterministic:

1. accumulate the magnitude of each owned glyph group's `x_advance`;
2. remember the latest Unicode-allowed boundary that is also caret-safe and fits the available inline advance;
3. when the line exceeds its width, emit that latest fitting boundary;
4. if no legal boundary fits, continue to the next legal boundary and emit a controlled overflow line;
5. never suppress a mandatory boundary;
6. never break inside a merged HarfBuzz group or at a boundary carrying unsafe-to-break evidence.

This stage does not invent emergency grapheme breaks. CSS `overflow-wrap`, `word-break`, hyphenation, and language-specific emergency policies remain later tailoring layers.

## Compact output

Each selected line uses one 16-byte `SelectedLineRecord`:

- 64-bit inline advance;
- 32-bit logical `cluster_limit`;
- 32-bit flags.

The first cluster is implied by the preceding record's `cluster_limit`, or zero for the first line. Flags distinguish soft wraps, mandatory boundaries, controlled overflow, the text-end line, and empty lines.

## Ligatures and RTL

Only owner clusters contribute glyph advance. Continuation clusters created by ligatures or other HarfBuzz cluster merges contribute zero, while the owner contributes the complete segment-local glyph span. Since merged interiors are rejected by the caret-safety map, cumulative widths remain exact at every selectable boundary.

RTL runs use the magnitude of signed HarfBuzz `x_advance` values. Logical line records remain in logical cluster order; visual bidi line reordering is not performed here.

## Memory and failure behavior

Construction uses one temporary 64-bit advance per logical cluster. A first deterministic pass counts selected lines, then the final vector is allocated exactly once and a second pass writes records. For `N` clusters and `L` selected lines:

- temporary memory: `8 * N` bytes;
- retained output: `16 * L` bytes;
- peak charged memory: `8 * N + 16 * L` bytes.

Both allocations use the caller's PMR resource and may be charged to the appended `LineSelectionMap` Resource Ledger class. Prior output is cleared before validation and remains empty after every failure.

## Certification fixture

The focused benchmark uses:

- 65,536 logical clusters;
- 2,048 alternating LTR/RTL shaping segments;
- 32,768 merged glyph groups;
- 65,537 logical boundaries;
- 2,048 units of available inline advance.

Expected output is 1,024 lines, comprising 960 soft wraps and 64 mandatory closures, with no overflow. Retained output is exactly 16,384 bytes and total charged peak memory is exactly 540,672 bytes.

## Explicit boundary

Z2D-2 does not construct layout fragments, trim or collapse whitespace, apply CSS line-breaking properties, perform hyphenation, reorder bidi runs visually per line, calculate baselines or block progression, place selections, perform hit testing, rasterize, or paint. Those remain Z2E and Z2F stages.
