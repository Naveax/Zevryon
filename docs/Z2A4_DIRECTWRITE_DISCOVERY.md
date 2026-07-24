# Z2A-4 — Windows DirectWrite discovery adapter

## Scope

Z2A-4 connects the Windows system font collection to the immutable Z2A-2
font generation contract. DirectWrite remains outside the portable core.

The adapter enumerates physical system faces, copies all native data needed by
the core into owned UTF-8 strings and Unicode ranges, releases every COM
object, and then calls `build_font_catalog_generation`.

## Native enumeration contract

The adapter:

- creates a shared DirectWrite factory;
- reads the legacy system font collection without forcing a collection refresh;
- skips simulated bold, oblique, and other synthetic `IDWriteFont` instances;
- selects `en-us` localized family and PostScript strings when present;
- otherwise selects the lexicographically smallest available locale name;
- converts UTF-16 with strict invalid-character rejection;
- requires every physical face to expose `IDWriteFontFace1` Unicode ranges;
- retrieves every face file through `IDWriteLocalFontFileLoader` and copies its
  absolute path before releasing the loader;
- sorts and deduplicates multi-file paths before constructing the identity;
- maps DirectWrite weight, stretch, and style into the core CSS-oriented record;
- records optional monospaced, color, and variable capabilities through newer
  DirectWrite interfaces when available.

No `IUnknown`, `IDWriteFontFile`, localized-string object, loader reference key,
or DirectWrite-owned buffer escapes `build_directwrite_generation`.

## Canonical identity

Each emitted face identity is a length-coded sequence containing:

1. all sorted unique local font-file paths;
2. DirectWrite face type;
3. face index;
4. native weight, stretch, and style;
5. optional PostScript name.

The identity does not use pointer values or lossy hashing. The Z2A-2 generation
builder sorts these byte-exact identities and assigns collision-free one-based
stable keys within that generation.

Repeated native entries with identical identity and metadata are collapsed.
An identity collision with different family, style, flags, preferred Script, or
coverage fails closed and publishes no generation.

## Unicode coverage

`IDWriteFontFace1::GetUnicodeRanges` is called through a bounded two-phase
buffer query. Returned ranges are validated, sorted, and merged. The adapter
counts covered code points and determines one dominant non-neutral Unicode
Script for fallback bucketing.

This preferred Script is only a fallback-ranking hint. It does not replace
OpenType Script/LangSys selection during shaping.

## Resource and publication model

Native discovery uses ordinary short-lived adapter allocations. Persistent
identity strings, family records, discovery records, catalog records, Script
buckets, and Unicode ranges are rebuilt inside the existing bounded immutable
`FontCatalogGeneration`.

Validation, native enumeration, allocation, local-file resolution, duplicate
conflict, discovery-budget, or catalog-budget failure leaves the caller output
null. The adapter never publishes a partial generation.

## Certification

The Windows integration test builds two generations from independent system
collection enumerations and requires exact equality of:

- semantic fingerprint;
- discovery records and family records;
- catalog faces and canonical coverage ranges;
- Script buckets and Script face identifiers.

It also requires every emitted face to carry the system flag, validates every
coverage slice, and proves discovery/catalog hard-budget failure is atomic.
Focused gates run the real system-font test under strict MSVC and MSVC
AddressSanitizer.

On the GitHub-hosted Windows Server 2025 image, strict Release and MSVC
AddressSanitizer produced the same certification:

- system families: **89**;
- enumerated font entries: **563**;
- simulated entries skipped: **291**;
- emitted physical faces: **272**;
- canonical coverage ranges: **114,134**;
- covered Unicode code points: **1,516,662**;
- duplicate canonical identities: **0**;
- variable faces: **102**;
- color faces: **1**;
- monospaced faces: **52**;
- persistent discovery bytes: **33,733**;
- discovery peak bytes: **51,141**;
- persistent catalog bytes: **924,272**;
- catalog peak bytes: **926,448**;
- fingerprint high: **13754863219733896334**;
- fingerprint low: **4041767920846461349**.

Exact system-font counts and memory values are runner-image dependent. The
portable contract requires internal consistency, deterministic repetition,
clean accounting, and bounded publication rather than one fixed OS inventory.

## Explicitly not implemented

Z2A-4 does not:

- open or map font files;
- parse OpenType, TTC, WOFF, cmap, variation, color, or layout tables;
- verify DirectWrite coverage against native cmap data;
- invoke DirectWrite font matching or fallback policy;
- create FreeType or HarfBuzz faces;
- shape glyphs, resolve variation coordinates, build caret maps, break lines,
  rasterize glyphs, or paint text;
- implement the macOS CoreText adapter.
