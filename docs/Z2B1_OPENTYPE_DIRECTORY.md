# Z2B-1 — Bounded SFNT/OpenType directory parser

## Scope

Z2B-1 establishes the untrusted-font binary boundary below shaping. It parses
only the OpenType/SFNT container and top-level table directory. It does not
interpret table payloads, select glyphs, call HarfBuzz or FreeType, or trust
platform discovery metadata as proof that a font file is structurally safe.

The parser accepts modern OpenType `0x00010000` TrueType-outline resources,
`OTTO` CFF/CFF2 resources, and TTC/OTC collection headers version 1.0 or 2.0.
Apple legacy `true` and `typ1` sfntVersion values are intentionally outside the
first browser-security profile.

## Zero-copy input and bounded output

The caller supplies an immutable byte span and a collection face index. Source
bytes are never copied. Successful output owns only sorted 16-byte
`OpenTypeTableRecord` values:

- four-byte tag;
- table checksum;
- file-relative offset;
- actual table length.

Permanent and temporary parser allocations use the appended
`FontResourceDirectory` Resource Ledger class. Temporary TTC protected ranges
and the offset-order overlap index are released before the published directory
is observed. Allocation failure publishes no directory.

## Structural validation

The parser validates:

- source size and policy bounds;
- supported SFNT and TTC versions;
- TTC face count, requested face, offset array, every face directory, and
  version-2 DSIG bounds/end position;
- four-byte directory and top-level table alignment;
- non-zero bounded table count;
- complete directory record bounds;
- printable ASCII table tags;
- strictly ascending and unique tags;
- non-zero table lengths;
- table offset/length overflow and source bounds;
- no table overlap with the TTC header or any face directory;
- no overlap between distinct selected-face tables;
- zero table padding to the next four-byte boundary;
- every table checksum, using zero bytes for `head.checksumAdjustment`;
- caller-selectable rejection of malformed stored search fields.

The stored `searchRange`, `entrySelector`, and `rangeShift` values are never
used for lookup. They are derived independently from `numTables`; malformed
stored values are counted and can be rejected by policy.

## Collection handling

All TTC table offsets are interpreted relative to the beginning of the
collection. Shared table regions are allowed across different faces. The
selected face cannot alias the collection header or any face directory.
Directory regions themselves must be distinct.

TTC version 2 accepts either three zero DSIG fields or a `DSIG` range that is
four-byte aligned, in bounds, and ends at the end of the collection.

## Checksum surface

Table checksums are unsigned sums of big-endian 32-bit words with implicit zero
padding. Verification of `head` treats bytes 8–11 (`checksumAdjustment`) as
zero, as required by the OpenType checksum procedure. Whole-font
`checksumAdjustment`, individual table semantics, and DSIG cryptographic
verification remain later validation layers.

## Consumer surface

The directory supports binary-search table lookup and zero-copy byte views for
later parsers. Initial named tags include `cmap`, `head`, `maxp`, `name`,
`OS/2`, `fvar`, `COLR`, and `CPAL`; no presence claim is made until the caller
checks the returned record.

## Tests and platform gates

The focused test suite covers:

- valid TrueType and shared-table TTC resources;
- zero-copy table lookup;
- permissive derived and strict stored search-field policy;
- out-of-range TTC face selection;
- non-printable, duplicate, and unsorted tags;
- misaligned and out-of-bounds tables;
- table/directory and table/table overlap;
- non-zero padding;
- table checksum corruption and `head` checksum special handling;
- one-byte hard-budget failure with no stale publication.

Permanent focused gates use strict GCC, strict AppleClang, Linux ASan/UBSan,
and MSVC AddressSanitizer.

## Explicit boundary

Z2B-1 does not parse SFNT table payloads, validate required-table combinations,
check `head.magicNumber` or whole-font `checksumAdjustment`, decode cmap,
resolve names, inspect variation axes, build glyph clusters, shape, rasterize,
or paint. Those capabilities must consume this bounded directory rather than
re-reading untrusted offsets independently.
