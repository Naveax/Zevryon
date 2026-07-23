# Z2A — Bounded font catalog and grapheme fallback

## Scope

Z2A establishes the platform-independent contract between font discovery and
future shaping. It deliberately does not call DirectWrite, CoreText, Fontconfig,
FreeType, or HarfBuzz.

Platform adapters provide immutable `FontFaceSeed` values. The core validates,
canonicalizes, sorts, and stores those values as a deterministic `FontCatalog`.
The fallback planner consumes the completed Unicode grapheme and script-run
surfaces and emits compact font-run boundaries for a later shaper.

## Adapter contract

Each discovered face must provide:

- a non-zero, collision-free `stable_key` that remains stable for the lifetime
  of one catalog generation;
- a collision-free `family_key` from the adapter's family-name interner;
- weight in 1..1000, width in 1..9, and an explicit slant;
- one shaping-oriented preferred Unicode Script;
- sorted, non-overlapping Unicode coverage ranges;
- optional variable, color, monospace, and system flags.

Z2A does not infer stable identifiers from lossy family-name hashing. Platform
adapters remain responsible for producing collision-free keys from native font
identity data such as file/reference identity plus face index.

## Catalog invariants

The catalog:

- rejects zero or duplicate stable keys;
- rejects malformed style metadata and Unicode ranges;
- sorts faces by stable key, independent of platform enumeration order;
- merges adjacent coverage ranges within each face;
- stores at most 32-byte face records plus canonical 8-byte coverage ranges;
- stores one compact 8-byte bucket descriptor for every Unicode Script;
- stores every face identifier exactly once in stable-key order inside its
  preferred-script bucket;
- validates bucket offsets, membership, ordering, and complete face coverage
  before fallback uses the index;
- uses a dedicated `FontCatalog` Resource Ledger class;
- publishes no partial catalog on validation or allocation failure.

The catalog owns no font file bytes, mapped tables, family-name strings, glyph
outlines, variation data, or shaping caches.

## Fallback order

For every complete grapheme cluster, selection is:

1. requested primary face, when it covers every scalar in the cluster;
2. requested preferred-family faces in caller order;
3. catalog faces ranked by script affinity, slant, width, weight, and stable key
   as the deterministic final tie-breaker;
4. explicit `Missing` when no face covers the complete cluster.

A face that covers only part of a grapheme cluster is never selected. This
prevents a base character and its combining marks, emoji ZWJ components, or
other extended-grapheme members from being silently split by fallback.

The optimized catalog scan is semantically equivalent to a complete stable-key
scan. Exact-script, neutral-script, and cross-script buckets use proven score
lower bounds. A bucket is skipped only when no member can beat or tie the current
best score. Equal-score stable-key ordering remains unchanged. A dedicated
oracle test covers the important case where a neutral upright face beats an
exact-script italic face because the complete score, rather than script tier
alone, decides the winner.

## Output and memory model

`FontFallbackBoundary` is 12 bytes:

- 4-byte cluster index;
- 4-byte font face identifier;
- 1-byte selection source;
- 3 reserved bytes.

Non-empty input produces one boundary per merged font run plus one final
sentinel at `cluster_count`. Consecutive missing clusters are represented as a
normal run with `kInvalidFontFaceId`.

The planner performs two deterministic passes. The first counts output runs and
collects statistics. The second recomputes the same choices, performs one exact
reserve, and publishes the completed plan. It does not allocate a dense
per-cluster face array, copy source text, copy grapheme boundaries, or copy
script runs. Output is charged to the separate `FontFallbackPlan` Resource
Ledger class.

## Certified 64 KiB / 256-face workload

The certification corpus contains Latin, combining marks, Greek, Cyrillic,
Arabic, Hebrew, Devanagari, Han, emoji, and an explicit uncovered scalar.
Catalog construction is outside the timed section. Every distribution performs
8 warmups and 128 measured complete fallback plans.

Final workload contract:

- input bytes: **65,536**;
- catalog faces: **256**;
- canonical coverage ranges: **257**;
- input codepoints: **27,119**;
- grapheme clusters: **22,599**;
- script runs: **20,339**;
- output runs: **22,599**;
- output boundaries: **22,600**;
- primary clusters: **2,260**;
- preferred-family clusters: **2,260**;
- script-match clusters: **13,560**;
- neutral-script clusters: **2,260**;
- missing clusters: **2,259**;
- coverage checks: **49,720**;
- exact fallback output: **271,200 bytes**;
- final fallback hard cap: **278,528 bytes / 272 KiB**;
- catalog current bytes: **12,680**;
- catalog peak gate: **16 KiB**.

Measured three-distribution result before final gate tightening:

- median P50: **5.170 ms**;
- median P95: **5.253 ms**;
- median P99: **5.351 ms**;
- worst maximum: **5.634 ms**.

Permanent gates are P95 <= 7 ms, P99 <= 8 ms, worst <= 12 ms, fallback peak <=
272 KiB, catalog peak <= 16 KiB, zero rejected reservations, and zero accounting
errors. The initial all-face baseline had P95 46.91 ms. Plane pruning, score
pruning, script buckets, and score-bound early exits reduced the same workload
by approximately 8.9x without changing selection semantics.

## Explicitly not implemented

Z2A does not implement:

- Windows DirectWrite discovery;
- macOS CoreText discovery;
- Linux Fontconfig discovery;
- OpenType/TTC/WOFF parsing;
- cmap extraction or native coverage verification;
- variable-axis resolution;
- font cache invalidation and generation switching;
- HarfBuzz shaping;
- glyph identifiers, advances, offsets, or glyph clusters;
- line breaking, caret maps, accessibility hit testing, paint, or rasterization.

A Z2A fallback plan is a shaping input contract, not final glyph output and not
proof that the complete browser text stack is implemented.
