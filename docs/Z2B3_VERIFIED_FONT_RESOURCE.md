# Z2B-3 — Immutable verified font resource generation

## Scope

Z2B-3 introduces an immutable ownership boundary between transient downloaded or
file-loaded bytes and later shaping/cache consumers. A resource is built once,
retains one exact private copy of the bytes under a hard memory limit, validates
one selected face, and publishes a shared read-only handle.

This is not yet a global cache. It is the cacheable immutable value type that a
later keyed store can retain, evict, and hand to HarfBuzz without re-copying or
re-validating the font.

## Ownership model

`VerifiedFontResource` owns, in member lifetime order:

- a private `ResourceLedger`;
- a `LedgerMemoryResource` assigned to `FontResourceBytes`;
- a PMR byte vector containing the retained font resource;
- an `SfntResourceView` pointing only into that byte vector;
- structural and integrity verification statistics;
- a caller-defined non-zero resource-generation ID.

The byte vector is populated exactly once before the view is built and is never
mutated afterwards. The view therefore remains stable for the full shared-object
lifetime. Caller source memory may be modified or released immediately after a
successful build.

## Resource accounting

`FontResourceBytes` is appended to `ResourceClass`, preserving all existing enum
ordinals. The resource hard limit is configured before the PMR vector performs
its first allocation.

A source larger than the hard limit fails before candidate construction. PMR
reservation rejection is reported as `OutputBudgetExceeded`; unrelated heap
failure is reported as `AllocationFailed`. The shared-pointer control block is
small allocator metadata outside the retained-byte ledger; all potentially
large font bytes are ledger-accounted.

## Validation and publication

`build_verified_font_resource` performs:

1. output, statistics, and error reset;
2. non-zero resource-ID and output-pointer validation;
3. source-size hard-limit precheck;
4. exact retained-byte reserve and copy;
5. selected-face SFNT/TTC structural parsing over retained bytes;
6. strict alignment, padding, table checksum, `head`, and whole-font integrity
   verification;
7. immutable view/stat publication through `shared_ptr<const ...>`.

Any failure destroys the candidate, releases retained bytes, and publishes no
resource. A previously retained reader handle is independent and remains valid.

## Error surface

Stable error categories distinguish:

- invalid API arguments;
- retained-byte budget exhaustion;
- allocator failure;
- SFNT/TTC parse failure;
- strict integrity failure.

Parser and integrity sub-errors, exact byte offsets, and affected table tags are
preserved without changing the source resource.

## Validation

The focused suite proves:

- a valid font succeeds with a hard limit exactly equal to its byte length;
- retained current/peak bytes and reservation count are exact;
- the view addresses the retained vector, not caller memory;
- caller mutation and release cannot alter retained data;
- copied reader handles retain the generation until the final reader drops it;
- malformed and checksum-corrupt replacement attempts publish no handle;
- failed replacement does not invalidate an existing reader;
- under-budget, zero-ID, invalid-face, and null-output failures are exact;
- ledger accounting remains clean and within the configured limit.

Focused CI runs strict GCC, AppleClang, and MSVC plus Linux and macOS
AddressSanitizer/UndefinedBehaviorSanitizer. Repository-wide CI supplies the
full Windows MSVC AddressSanitizer matrix and existing parser, integrity,
HarfBuzz, discovery, Unicode, ZENITH, and giant-record regressions.

## Explicit boundary

Z2B-3 does not:

- assign content hashes or discovery identities;
- map files without copying;
- implement a global cache, LRU, eviction, or concurrent lookup table;
- pass the immutable handle directly to HarfBuzz;
- parse semantic OpenType tables;
- rasterize or paint.

The next slice can extend shaping requests with this immutable handle and retain
the raw-byte path only as the fail-closed uncached fallback.
