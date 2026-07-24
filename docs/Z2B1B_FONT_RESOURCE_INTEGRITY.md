# Z2B-1B — Allocation-free SFNT integrity verification

## Scope

Z2B-1B hardens the merged non-owning `SfntResourceView` without creating a
second directory parser or a persistent directory object. It applies optional
OpenType structural-integrity policy to an already validated single-font or
TTC/OTC face view.

The verifier performs no heap allocation, retains no source ownership, and does
not mutate the resource view or source bytes.

## Verified properties

The strict default policy verifies:

- the selected sfnt directory begins at a four-byte aligned offset;
- every table begins at a four-byte aligned offset;
- bytes between each table's actual length and its four-byte padded length are
  present and zero;
- every table checksum matches the checksum stored in its directory record;
- the `head` table exists and is long enough to contain `checkSumAdjustment`;
- the `head` table checksum is calculated with the four adjustment bytes treated
  as zero;
- a standalone font's complete uint32 checksum equals `0xB1B0AFBA`.

The OpenType 1.9.1 font-file specification defines four-byte table alignment,
zero padding, directory table checksums, and the `head.checkSumAdjustment`
calculation. The `head` specification also states that whole-font adjustment is
invalidated when a font is embedded in a collection. Therefore TTC/OTC faces
still receive per-table checksum verification, but whole-file adjustment is
explicitly skipped and reported in statistics.

## Policy controls

`SfntIntegrityOptions` independently controls:

- required `head` presence;
- four-byte directory/table alignment;
- zero table padding;
- directory table checksums;
- standalone whole-font checksum adjustment.

The strict defaults are intended for untrusted browser font resources. Tests
may selectively disable expensive or compatibility-sensitive checks without
changing the underlying parser contract.

## Checksum implementation

`calculate_sfnt_checksum`:

- consumes bytes as big-endian uint32 words;
- pads a final partial word with zeros;
- uses modulo-2^32 addition;
- supports one caller-selected byte range treated as zero;
- allocates no memory and throws no exceptions.

The `head` table uses zero range `[8, 12)`. Other tables are summed unchanged.
For a standalone font, the complete source span is summed with the stored
adjustment present and must equal `0xB1B0AFBA`.

## Failure model

Every failure returns a stable allocation-free error containing:

- error kind;
- exact source byte offset;
- table index and tag;
- expected and actual values where relevant;
- static diagnostic text.

Statistics and error outputs are reset before verification. The input view is
immutable and cannot be partially modified.

## Validation

Independent test fixtures calculate directory and whole-font checksums without
calling the production checksum function. Coverage includes:

- strict standalone TrueType success;
- TTC table-checksum success with arbitrary ignored adjustment;
- exact payload, padding, alignment, and checksum statistics;
- final partial-word zero padding;
- misaligned table rejection;
- non-zero padding and missing terminal padding;
- payload checksum corruption;
- isolated `checkSumAdjustment` corruption that leaves the `head` table
  checksum valid but breaks the whole-font checksum;
- missing and truncated `head` tables;
- policy-controlled disabling of padding and checksum checks;
- invalid-view failure with atomic output reset.

Focused CI compiles and runs both the container parser and integrity verifier
under strict GCC, AppleClang, and MSVC, plus Linux and macOS AddressSanitizer and
UndefinedBehaviorSanitizer. Repository-wide CI continues to provide full
Windows MSVC AddressSanitizer coverage.

## Explicit boundary

Z2B-1B does not:

- parse semantic fields in `head`, `maxp`, `cmap`, `name`, `OS/2`, variation,
  color, or OpenType Layout tables;
- verify TTC DSIG cryptography;
- cache verified resources;
- connect discovery identities to file loading;
- change the HarfBuzz shaping request contract;
- rasterize or paint glyphs.

The next integration slice can require parser and integrity success before
caller-owned bytes enter the optional HarfBuzz backend.
