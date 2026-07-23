# Z2A-2 — Deterministic font discovery generations

## Scope

Z2A-2 defines the immutable boundary between native platform font enumeration
and the merged Z2A catalog/fallback core. DirectWrite, CoreText, and Fontconfig
adapters will populate `FontDiscoveryFace` records, but this layer contains no
platform API calls and parses no font files.

## Identity contract

Every adapter face supplies two byte-exact, non-empty, NUL-free UTF-8 strings:

- `platform_identity`: a collision-free native identity for one concrete face;
- `family_name`: the adapter's canonical family name.

Malformed, overlong, surrogate, and out-of-range UTF-8 is rejected. No lossy
hash is used to create stable keys.

Faces are sorted lexically by `platform_identity`. Stable keys are the one-based
positions in that canonical order, making collisions impossible inside one
generation. Unique family names are sorted lexically and receive one-based
family keys. Platform enumeration order therefore cannot change keys or the
semantic fingerprint.

Stable and family keys are generation-local. Persistent caches must include the
generation fingerprint or byte-exact platform identity; they must not assume a
numeric key is stable across different semantic generations.

## Immutable generation ownership

`FontCatalogGeneration` owns:

- one compact UTF-8 pool containing canonical identities and unique families;
- 16-byte discovery records;
- 12-byte family records;
- the complete bounded `FontCatalog`;
- independent `FontDiscoverySnapshot` and `FontCatalog` ledger resources;
- a non-zero generation ID;
- a two-word deterministic semantic fingerprint.

The generation owns its PMR resources. Readers hold
`shared_ptr<const FontCatalogGeneration>` snapshots, so an old catalog remains
valid until its last reader releases it even after a newer generation is
published.

## Fingerprint contract

The fingerprint includes canonical identity, family, style, Script, flags, and
all coverage ranges for every face. It excludes the generation ID, so identical
font state discovered under a newer sequence number produces the same
fingerprint.

The fingerprint is non-cryptographic. It is used for deterministic change
recognition and avoiding unnecessary fallback/shaping cache invalidation. It is
not a font-file integrity hash, security identity, signature, or adversarial
collision boundary.

## Publication contract

`FontCatalogGenerationStore` uses the standard atomic shared-pointer free
operations through a portable wrapper. This works on libstdc++, libc++, and
MSVC without requiring `atomic<shared_ptr>` class specialization.

Publication outcomes are:

- `Published`: a newer, semantically changed generation replaced the current
  snapshot;
- `InvalidCandidate`: null, unbounded, or accounting-invalid candidate;
- `StaleGeneration`: generation ID is not newer than the current generation;
- `IdenticalSnapshot`: generation ID is newer but the semantic fingerprint is
  identical, so the current snapshot remains installed.

A dedicated multi-reader stress test proves that readers observe either the
complete old generation or the complete new generation during publication.
No partial string pool, record array, catalog, or generation metadata is ever
visible.

## Failure model

Generation construction is publish-on-success:

- invalid identity or family UTF-8 publishes nothing;
- duplicate platform identity publishes nothing;
- 32-bit index or pool overflow publishes nothing;
- discovery budget failure publishes nothing;
- nested catalog validation or budget failure publishes nothing;
- output pointers are cleared before construction begins.

Discovery allocations are charged to `FontDiscoverySnapshot`. Catalog records,
coverage, and Script buckets remain charged to `FontCatalog`. The fixed
`shared_ptr` control object is not part of either variable-size data budget.

## Explicitly not implemented

Z2A-2 does not implement:

- DirectWrite, CoreText, or Fontconfig enumeration;
- native identity construction rules for any platform;
- file watching or platform change notifications;
- OpenType, TTC, WOFF, cmap, or variation parsing;
- cryptographic file hashing;
- HarfBuzz shaping or glyph clusters;
- font fallback cache invalidation beyond atomic generation replacement;
- line breaking, caret maps, accessibility, paint, or rasterization.

The next partitions are the three native discovery adapters, followed by
font-file metadata verification and shaping.
