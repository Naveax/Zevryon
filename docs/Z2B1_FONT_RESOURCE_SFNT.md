# Z2B-1 — Allocation-free SFNT/TTC resource view

## Scope

Z2B-1 begins the portable font-resource layer after the completed Z2A platform
discovery adapters. It validates the top-level OpenType `sfnt` container and
exposes one non-owning face view over caller-owned immutable bytes.

This slice does not allocate, map files, retain operating-system objects, parse
individual OpenType tables, or shape text.

## Supported containers

The parser accepts:

- a single TrueType-outline sfnt with version `0x00010000`;
- a single CFF/CFF2-outline sfnt with version tag `OTTO`;
- OpenType Font Collection headers `ttcf` version 1.0 or 2.0;
- caller-selected collection face indices.

Legacy Apple scaler tags `true` and `typ1` are rejected in this OpenType-only
boundary. WOFF and WOFF2 are separate compressed containers and remain outside
this slice.

## Directory contract

The parser reads all multi-byte fields as big-endian values and validates:

- complete single-font, TTC v1, or TTC v2 headers;
- non-zero collection face counts bounded to 65,535;
- every collection face offset points beyond the collection header and has a
  complete 12-byte sfnt directory header;
- TTC v2 DSIG metadata is either empty or a bounded `DSIG` range;
- one to 1,024 table records per selected face;
- complete `12 + numTables * 16` directory extent;
- printable four-byte table tags;
- strictly ascending, duplicate-free table tags;
- every table offset and length remains inside the resource;
- no table overlaps the TTC header, selected face directory, or another table.

The 1,024-table limit bounds adversarial CPU cost. Real OpenType fonts normally
contain far fewer top-level tables.

## Search parameters

The OpenType table directory contains `searchRange`, `entrySelector`, and
`rangeShift`. The OpenType 1.9.1 specification recommends that parsers derive
these values independently from `numTables` rather than trusting file values,
because incorrect hints can become an attack surface.

Zevryon therefore:

- never uses the stored values for lookup;
- validates the table ordering directly;
- performs its own binary search over the validated records;
- reports whether the stored hints match the independently derived values;
- does not reject an otherwise valid font solely for stale search hints.

Reference: Microsoft OpenType 1.9.1, “OpenType font file”.

## Non-owning view

`SfntResourceView` stores only:

- the caller byte span;
- selected directory offset;
- face count and selected face index;
- table count;
- container kind and outline flavor.

It provides:

- indexed validated table-record access;
- binary lookup by four-byte tag;
- exact table byte slices.

The caller must keep the original byte span alive and immutable. Failure resets
an existing output view before returning, so stale data cannot survive a failed
reopen.

## Security properties

The implementation performs no heap allocation. All offsets, products, and
extent calculations are checked before use. Table end calculations use widened
integer arithmetic. Every error reports a stable error kind, byte offset, and
record index without allocating an error string.

Malformed collection offsets, unsupported versions, truncated directories,
invalid tags, duplicate or unsorted records, escaped ranges, directory overlap,
and table overlap fail closed.

## Validation

The focused test suite covers:

- valid TrueType and CFF single-font resources;
- stale search hints that are reported but not trusted;
- TTC version 1 face selection and mixed face flavors;
- TTC version 2 empty and invalid DSIG metadata;
- every strict prefix truncation before the final required table byte;
- invalid face indices and output reset after failure;
- zero and over-limit table counts;
- non-printable, duplicate, and descending tags;
- out-of-resource, directory-overlapping, and table-overlapping ranges;
- invalid collection versions, counts, and face offsets.

Focused CI builds and runs the production target with strict GCC, AppleClang,
and MSVC warnings-as-errors, plus Linux and macOS AddressSanitizer and
UndefinedBehaviorSanitizer. The repository-wide Windows sanitizer matrix also
rebuilds this source through the main core target.

## Explicit boundary

Z2B-1 does not:

- open, map, cache, or watch font files;
- compute whole-font or table checksums;
- parse `head`, `maxp`, `cmap`, `name`, `OS/2`, `hhea`, `hmtx`, `loca`, `glyf`,
  `CFF `, `CFF2`, `fvar`, `avar`, `STAT`, `COLR`, `CPAL`, `GSUB`, `GPOS`, or
  `GDEF`;
- validate platform discovery coverage against native `cmap` data;
- create FreeType or HarfBuzz objects;
- select fallback, shape glyphs, rasterize, or paint.

The next Z2B slice should build bounded table-specific readers on top of this
validated non-owning resource view.
