# Z2A-5 — macOS CoreText discovery adapter

## Scope

Z2A-5 connects the macOS available-font collection to the immutable Z2A-2
font-generation contract. CoreText and CoreFoundation remain outside the
portable core.

The adapter enumerates normalized descriptors from
`CTFontCollectionCreateFromAvailableFonts`, creates one temporary `CTFont` per
descriptor, copies all required metadata into owned C++ storage, releases every
CF/CT object, and then calls `build_font_catalog_generation`.

## Physical identity

Only descriptors with a file URL are emitted in this slice. Non-file
CoreText descriptors are counted and skipped rather than assigned an unstable
process-local identity.

The collision-free length-coded identity contains:

- the POSIX file-system path;
- the PostScript name;
- the family name;
- symbolic traits;
- converted weight, width, and slant;
- sorted variation-axis coordinate identifiers and exact floating-point bits.

No pointer, CFHash value, localized display-order position, or lossy digest is
part of identity. Repeated identical identities are collapsed. Conflicting
metadata for one identity fails closed and publishes no generation.

## Metadata conversion

CoreText normalized traits are converted to the existing CSS-oriented catalog
fields:

- weight in the 1–1000 range;
- width in the nine-step CSS stretch range;
- upright, italic, or oblique slant;
- system, monospaced, color, and variable flags.

Variable capability is detected through `CTFontCopyVariationAxes`. Current
variation coordinates are included in identity. OpenType axis ranges and named
instances remain a Z2B font-resource parsing concern.

## Unicode coverage

`CTFontCopyCharacterSet` supplies the nominal Unicode cmap coverage. The first
correctness implementation checks only populated Unicode planes and uses
`CFCharacterSetIsLongCharacterMember` for UTF-32 membership, producing sorted
maximal ranges over U+0000–U+10FFFF. The same canonical coverage computes the
dominant non-neutral Unicode Script.

This intentionally avoids undocumented assumptions about CoreFoundation bitmap
serialization. A later Z2A-5B optimization may replace the membership scan only
if byte-exact coverage equivalence is proven against this reference path.

## Ownership and publication

No font descriptor, font, character set, trait dictionary, variation array,
string, URL, or borrowed CoreFoundation buffer escapes
`build_coretext_generation`.

Collection failure, invalid UTF-8 conversion, malformed traits or variations,
empty coverage, duplicate conflict, allocation failure, discovery-budget
failure, or catalog-budget failure publishes no generation.

## Validation

The focused macOS workflow performs:

- strict AppleClang compilation;
- real available-font enumeration;
- complete immutable-generation comparison across two independent
  enumerations;
- identity namespace and stable face-ID checks;
- Resource Ledger and hard-limit checks;
- atomic one-byte discovery-budget failure;
- AppleClang AddressSanitizer and UndefinedBehaviorSanitizer execution.

## Explicit boundary

This adapter does not parse OpenType/TTC/WOFF tables, extract native face
indices unavailable through the descriptor identity, use CoreText fallback or
cascade policy, shape glyphs, build caret maps, rasterize fonts, or paint text.
Those remain Z2B and later rendering milestones.
