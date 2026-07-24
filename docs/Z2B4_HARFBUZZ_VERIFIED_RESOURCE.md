# Z2B-4 — HarfBuzz immutable verified-resource input

## Scope

Z2B-4 allows the public HarfBuzz shaping request to consume the immutable
`VerifiedFontResource` introduced by Z2B-3. The retained handle reuses the
already copied, structurally parsed, and integrity-verified font bytes without
repeating whole-resource validation for every shaped segment.

The existing raw-byte path remains available as a fail-closed uncached fallback
and continues to perform inline SFNT/TTC parsing and strict integrity checks on
every call.

## Input contract

`HarfBuzzShapingRequest` appends one optional
`shared_ptr<const VerifiedFontResource>` field. Appending the field preserves
existing aggregate-initializer source compatibility.

Exactly one input form is allowed:

- raw mode: non-empty `font_bytes`, null resource handle;
- retained mode: empty `font_bytes`, non-null resource handle.

In retained mode, `face_index` must equal the face selected by the resource.
Supplying both forms or a mismatched face fails as `InvalidInput` before any
HarfBuzz object is created.

## Retained-resource invariants

Before backend dispatch the wrapper checks:

- the selected resource view is valid;
- retained bytes are non-empty;
- the view points at the retained byte vector;
- the private resource ledger is accounting-clean;
- retained bytes remain within their hard limit.

The wrapper takes a local shared-pointer copy so the immutable resource remains
alive through the complete synchronous HarfBuzz call. It then creates a private
backend request whose byte span refers to the retained storage and whose public
resource field is cleared.

No byte copy, SFNT reparse, table checksum, padding scan, or whole-font checksum
is performed in retained mode.

## Raw fallback

When no immutable resource is supplied, the existing Z2B-2 boundary remains
unchanged:

1. parse the requested SFNT/TTC face;
2. verify alignment, zero padding, table checksums, `head`, and whole-font
   adjustment;
3. dispatch to the private HarfBuzz backend only after success.

This preserves the secure one-shot API for callers that do not yet maintain
resource generations.

## Statistics

Successful shaping now reports:

- immutable resource ID, or zero for raw mode;
- whether an immutable resource was consumed;
- whether inline font verification was performed;
- the original parse and integrity evidence in both modes.

Glyph, cluster, position, safety-flag, and Resource Ledger statistics are
otherwise unchanged.

## Validation

The real DejaVu Sans integration test requires:

- raw and retained paths produce byte-identical `ShapedGlyph` records;
- output glyph count and stored validation evidence agree;
- retained shaping succeeds after caller source bytes are cleared and released;
- retained mode reports the exact resource ID and no inline verification;
- raw mode reports inline verification and no resource ID;
- raw-plus-resource ambiguity fails atomically;
- face-index mismatch fails atomically and preserves the requested index;
- failed requests clear previously published glyph output and statistics.

The existing Latin, Arabic, Hebrew, combining-mark, and Devanagari tests remain
mandatory.

## Benchmark contract

The 16 KiB benchmark supports two modes:

- `uncached_full_call` — parser, integrity verification, and shaping each call;
- `verified_resource_call` — immutable resource built once outside timing,
  shaping only inside the measured loop.

Each of three independent distributions emits Latin, Arabic, and Devanagari
rows. CI requires equivalent glyph counts, advances, safety flags, output bytes,
and ledger accounting between modes. The first certified run establishes the
permanent retained-resource latency gates.

## Explicit boundary

Z2B-4 does not:

- maintain a global keyed cache or LRU;
- reuse HarfBuzz blob, face, or font objects across calls;
- map files or resolve discovery paths;
- parse semantic OpenType tables outside HarfBuzz;
- rasterize or paint.

A later cache layer may retain verified resources and backend font objects while
preserving this public mutually-exclusive input contract.
