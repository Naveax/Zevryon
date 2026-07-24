# Z2B-8 — Platform Identity Load Locator

## Purpose

`parse_font_load_locator` converts one immutable discovery `platform_identity` string into an allocation-free load locator. It is the structural boundary between platform discovery snapshots and later file/resource resolution.

The parser does not open files, join paths, create platform objects, or guess an unresolved face index.

## Shared field encoding

All three discovery adapters encode fields as:

```text
<canonical decimal byte length>:<raw UTF-8 bytes>|
```

The parser validates:

- non-empty decimal length;
- decimal digits only;
- no leading zero except the value `0`;
- `size_t` overflow;
- payload bounds;
- the terminating pipe;
- no embedded null bytes;
- no trailing bytes after the platform-specific field sequence;
- a maximum of 4,096 parsed fields.

Delimiters inside field payloads are preserved because field boundaries are length-based.

## Fontconfig identity

Expected fields after `fontconfig|`:

1. sysroot;
2. file path;
3. face index;
4. PostScript name;
5. variation descriptor.

The file path is required and the face index must be canonical unsigned 32-bit decimal. The locator exposes sysroot and file path separately; this slice does not assume how Fontconfig sysroot semantics should be joined.

Capability: `SingleFileWithFaceIndex`.

## DirectWrite identity

Expected fields after `directwrite|`:

1. file count;
2. one field per local file path;
3. face type;
4. face index;
5. weight;
6. stretch;
7. style;
8. PostScript name.

A zero file count is rejected. A single-file identity exposes the path and explicit face index. A multi-file identity is structurally valid but exposes no portable single path.

Capabilities:

- one file: `SingleFileWithFaceIndex`;
- more than one file: `MultiFile`.

## CoreText identity

Expected fields after `coretext|`:

1. file path;
2. PostScript name;
3. CSS weight;
4. CSS width;
5. slant;
6. variation count;
7. one signed identifier and one unsigned value-bit field per variation.

The parser validates weight `1..1000`, width `1..9`, slant `0..2`, canonical signed 64-bit variation identifiers, unsigned 64-bit value bits, and complete variation pairs.

The current CoreText identity does not carry an sfnt face index. The parser therefore refuses to invent one.

Capability: `SingleFileFaceIndexUnresolved`.

## Lifetime and allocation contract

The returned strings are `std::string_view` values into the caller-owned identity. The caller must retain the immutable identity, normally through `FontCatalogGeneration`, for the locator lifetime.

The production parser:

- performs no heap allocation;
- uses bounded linear scans;
- publishes no owned strings;
- resets output on every failure;
- reports byte offset and field index for malformed input.

## Certified behavior

The focused suite covers:

- real adapter field order for Fontconfig, DirectWrite, and CoreText;
- delimiters inside length-prefixed fields;
- single-file and multi-file DirectWrite capability separation;
- unresolved CoreText face-index behavior;
- all proper truncation prefixes of a valid identity;
- malformed length syntax and overflow;
- missing separators and terminators;
- embedded null bytes;
- empty required paths;
- unsigned and signed numeric overflow;
- noncanonical leading zeros and negative zero;
- incomplete CoreText variation pairs;
- trailing data;
- output reset after failure;
- strict GCC, AppleClang, and MSVC builds;
- Linux and macOS ASan/UBSan;
- three independent one-million-identity mixed-platform distributions.

## Permanent performance gates

For one million mixed Fontconfig, DirectWrite, and CoreText parses on the hosted Ubuntu runner:

- throughput must be at least 3,000,000 identities per second;
- elapsed time must be at most 350 ms.

## Explicit boundary

This slice does not join Fontconfig sysroots, load files, resolve DirectWrite multi-file faces, infer CoreText face indices, persist locators, create HarfBuzz objects, rasterize, or paint.
