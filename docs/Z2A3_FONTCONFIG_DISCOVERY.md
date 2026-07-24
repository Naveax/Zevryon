# Z2A-3 — Linux Fontconfig discovery adapter

## Scope

Z2A-3 is the first native platform adapter above the deterministic Z2A-2
generation boundary. It uses Fontconfig only on Linux/Unix builds where
`pkg-config` exposes the `fontconfig` package. The portable MassiveDoc core has
no Fontconfig include, symbol, or link dependency.

The adapter calls `FcInitLoadConfigAndFonts` and `FcFontList` with a restricted
object set. Every Fontconfig-owned pattern, string, and charset is consumed and
copied before the call returns; native pointers never escape into the core.

## Native identity

One concrete face identity is length-coded from:

- Fontconfig sysroot;
- `FC_FILE`;
- `FC_INDEX`;
- `FC_POSTSCRIPT_NAME` when present;
- `FC_FONT_VARIATIONS` when present.

Length coding prevents delimiter collisions. Identical duplicate patterns are
deduplicated. The same identity with conflicting metadata fails closed instead
of silently choosing one pattern.

## Metadata conversion

The adapter converts:

- Fontconfig weight through `FcWeightToOpenTypeDouble`, clamped to CSS 1–1000;
- Fontconfig width constants to the existing nine-step width contract;
- Roman/italic/oblique slant to `FontSlant`;
- `FC_VARIABLE`, `FC_COLOR`, and monospace/charcell spacing to face flags;
- the first canonical family value to byte-exact UTF-8 family storage.

## Coverage and Script

`FC_CHARSET` is traversed page by page with `FcCharSetFirstPage` and
`FcCharSetNextPage`. Set bits are merged into sorted Unicode coverage ranges.
The same pass counts non-neutral Unicode Script values and assigns the dominant
Script as the shaping-oriented preferred Script. Empty coverage is rejected.

## Generation handoff

The resulting owned adapter records are converted to temporary
`FontDiscoveryFace` views and passed directly to
`build_font_catalog_generation`. The generation copies all identity, family,
coverage, and catalog data before adapter temporaries are destroyed.

Discovery/catalog budget failure, malformed native values, missing required
properties, and conflicting duplicates publish no generation.

## Validation

The focused Linux workflow installs the Fontconfig development package and
runs:

- strict GCC compilation;
- real system-font enumeration twice;
- exact fingerprint equality across unchanged enumerations;
- strict canonical identity ordering and stable-key alignment;
- coverage and accounting checks;
- ASan/UBSan integration testing.

## Explicit boundary

This adapter does not open font files, parse OpenType tables, create FreeType
faces, shape glyphs, or apply Fontconfig matching/substitution as the browser's
fallback policy. Fontconfig supplies discovery metadata; Zevryon's bounded
catalog and fallback planner remain authoritative after ingestion.
