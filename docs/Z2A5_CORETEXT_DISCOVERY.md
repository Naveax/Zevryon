# Z2A-5 — macOS CoreText discovery adapter

## Scope

Z2A-5 connects the CoreText font collection available to the current
application to the immutable Z2A-2 generation contract. CoreText and Core
Foundation remain outside the portable core.

The adapter copies every persistent value into owned UTF-8 strings, variation
coordinates, or canonical Unicode ranges before releasing the native objects.

## Native enumeration contract

The adapter:

- creates the collection returned by `CTFontCollectionCreateFromAvailableFonts`;
- requests normalized matching descriptors from the collection;
- creates one `CTFont` for each addressable descriptor;
- copies an absolute POSIX file path from `kCTFontURLAttribute`;
- copies PostScript and family names with exact UTF-8 conversion;
- reads normalized weight, width, and slant traits;
- reads symbolic monospaced, color-glyph, italic, and bold traits;
- marks a face variable when `CTFontCopyVariationAxes` returns one or more axes;
- copies current variation coordinates into canonical identity data;
- copies the font character set and immediately converts it into owned ranges.

Descriptors that do not expose an absolute file URL, valid names, valid traits,
variation coordinates, or a non-empty character set are skipped and counted.
No `CFTypeRef`, `CTFontRef`, descriptor, URL, character set, or bitmap buffer
escapes `build_coretext_generation`.

## Canonical identity

Each emitted face identity is a length-coded sequence containing:

1. absolute POSIX file path;
2. PostScript name;
3. mapped weight, width, and slant;
4. sorted variation-axis identifiers and exact double bit patterns.

The identity does not use pointer values, descriptor enumeration order, locale
order, or lossy hashing. Z2A-2 sorts the byte-exact identities and assigns
collision-free one-based stable keys within the generation.

Repeated descriptors with identical identity and metadata are collapsed. An
identity collision with different family, style, flags, preferred Script, or
coverage fails closed and publishes no generation.

## Unicode coverage

CoreText character sets are converted with
`CFCharacterSetCreateBitmapRepresentation`.

The documented representation contains:

- one 8 KiB bitmap for the Basic Multilingual Plane;
- zero to sixteen additional plane-index bytes followed by 8 KiB bitmaps.

The adapter validates segment length, plane index uniqueness, and Unicode plane
bounds. It reads set bits directly, removes UTF-16 surrogate code points,
merges adjacent scalar ranges, counts covered code points, and determines one
dominant non-neutral Unicode Script for fallback bucketing.

This preferred Script is only a fallback-ranking hint. It does not replace
OpenType Script/LangSys selection during shaping.

## Trait mapping

CoreText normalized traits are mapped deterministically into the portable
contract:

- negative weight values map from CSS weight 400 toward 100;
- positive weight values map from 400 toward 900;
- the symbolic bold trait floors the result at 700;
- normalized width maps into the portable 1..9 width range;
- symbolic italic maps to `Italic`;
- non-zero normalized slant without italic maps to `Oblique`.

All emitted faces carry the system flag. Monospaced, color, and variable flags
are derived from CoreText traits and variation axes.

## Resource and publication model

Native discovery uses short-lived adapter allocations. Persistent identity
strings, family records, discovery records, catalog records, Script buckets,
and Unicode ranges are rebuilt inside the existing bounded immutable
`FontCatalogGeneration`.

Collection creation, allocation, duplicate conflict, discovery-budget, or
catalog-budget failure leaves the caller output null. The adapter never
publishes a partial generation.

## Certification

The macOS integration test builds two generations from independent CoreText
collection enumerations and requires exact equality of:

- semantic fingerprint;
- discovery records and family records;
- catalog faces and canonical coverage ranges;
- Script buckets and Script face identifiers.

It validates every coverage slice, requires all emitted faces to carry the
system flag, confirms surrogate exclusion, accounts skipped descriptors, and
proves discovery/catalog hard-budget failure is atomic.

Focused gates run the real system-font test under strict AppleClang and under
AppleClang AddressSanitizer plus UndefinedBehaviorSanitizer. Exact system-font
counts and memory values are runner-image dependent and are stored in workflow
artifacts rather than hard-coded into the portable contract.

## Explicitly not implemented

Z2A-5 does not:

- open or map font files;
- parse OpenType, TTC, WOFF, cmap, variation, color, or layout tables;
- verify CoreText character sets against native cmap data;
- invoke CoreText cascade or matching policy for document text;
- create FreeType or HarfBuzz faces;
- shape glyphs, resolve final variation coordinates, build caret maps, break
  lines, rasterize glyphs, or paint text.
