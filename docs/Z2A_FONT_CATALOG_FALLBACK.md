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
- uses a dedicated `FontCatalog` Resource Ledger class;
- publishes no partial catalog on validation or allocation failure.

The catalog owns no font file bytes, mapped tables, family-name strings, glyph
outlines, variation data, or shaping caches.

## Fallback order

For every complete grapheme cluster, selection is:

1. requested primary face, when it covers every scalar in the cluster;
2. requested preferred-family faces in caller order;
3. all catalog faces ranked by script affinity, slant, width, weight, and stable
   key as the deterministic final tie-breaker;
4. explicit `Missing` when no face covers the complete cluster.

A face that covers only part of a grapheme cluster is never selected. This
prevents a base character and its combining marks, emoji ZWJ components, or
other extended-grapheme members from being silently split by fallback.

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

## Current complexity boundary

The initial general fallback path is intentionally simple and auditable:

`O(clusters × catalog_faces × cluster_codepoints × coverage_lookup)`

Primary and preferred-family hits exit before the catalog scan. A synthetic
64 KiB / 256-face benchmark records coverage checks, latency distribution, and
exact output allocation. Script buckets, coverage-page indexes, and reusable
fallback caches will only be added after this baseline identifies the dominant
cost; they must not weaken deterministic ranking or hard-budget behavior.

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
