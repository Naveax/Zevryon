# Z2A-4 — Windows DirectWrite discovery adapter

## Scope

Z2A-4 is the Windows-native discovery adapter above the deterministic Z2A-2
catalog-generation boundary. The portable core remains free of Windows and
DirectWrite headers, symbols, and link dependencies.

The adapter enumerates the system collection through `IDWriteFactory` and
copies every native value into owned C++ storage before building an immutable
`FontCatalogGeneration`. No COM interface, localized string, file reference,
or Unicode-range buffer escapes the call.

## Native identity

A concrete face identity is length-coded from:

- every sorted local font-file path returned by the face;
- the face index within its file collection;
- canonical English family and face names;
- DirectWrite weight, stretch, style, and simulation flags.

Length coding prevents delimiter collisions. Enumeration order is not part of
identity. Identical duplicate faces are deduplicated; conflicting metadata for
one identity fails closed.

The first implementation accepts only DirectWrite system faces backed by the
built-in local font-file loader. A custom or remote loader is rejected instead
of receiving an unstable process-local identity.

## Metadata conversion

The adapter converts:

- DirectWrite weight to the existing CSS 1–1000 field;
- DirectWrite stretch to Zevryon's nine-step width field;
- normal, italic, and oblique style to `FontSlant`;
- `IDWriteFont1::IsMonospacedFont` to the monospace flag;
- every emitted face to the system-font flag;
- simulation flags into the collision-free platform identity.

Variable-axis and color-glyph capabilities are intentionally not inferred from
collection metadata in this slice. Z2B will validate OpenType tables and expose
those capabilities from the parsed font resource.

## Coverage and Script

`IDWriteFontFace1::GetUnicodeRanges` provides the native Unicode coverage.
Ranges are sorted, validated against U+10FFFF, merged, and copied into core
coverage records. The same canonical coverage computes the dominant
non-neutral Unicode Script used by the fallback planner.

The adapter requires DirectWrite 1 coverage support. A face that cannot expose
bounded Unicode ranges is rejected; per-codepoint probing is not used as a
silent fallback.

## Generation handoff

Owned adapter records are converted to temporary `FontDiscoveryFace` views and
passed directly to `build_font_catalog_generation`. The generation copies all
identity, family, coverage, and catalog data before adapter temporaries are
destroyed.

Factory failure, unavailable local file identity, invalid localized UTF-16,
invalid coverage, duplicate conflicts, allocation failure, and nested
snapshot/catalog failure publish no generation.

## Validation

The focused Windows workflow performs:

- strict MSVC `/W4 /WX /permissive-` compilation;
- real system-font enumeration twice;
- byte-exact fingerprint, discovery-record, family, face, coverage, and script
  index equality across unchanged runs;
- identity namespace and stable face-ID checks;
- hard-limit and Resource Ledger accounting checks;
- MSVC AddressSanitizer execution.

## Explicit boundary

This adapter does not parse OpenType tables, create a glyph-shaping cache,
select fallback through DirectWrite matching policy, shape text, rasterize
fonts, or implement the CoreText adapter. DirectWrite supplies Windows system
font discovery metadata; Zevryon's catalog and fallback planner remain
authoritative after ingestion.
