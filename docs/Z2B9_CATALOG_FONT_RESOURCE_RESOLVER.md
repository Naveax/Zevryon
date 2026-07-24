# Z2B-9 — Catalog Face Resource Resolver

## Purpose

`resolve_catalog_font_resource` joins the immutable discovery/catalog generation to the verified font resource pipeline:

```text
FontFaceId
  -> generation-owned platform identity
  -> allocation-free load locator
  -> UTF-8 filesystem path
  -> bounded stable file loader
  -> content-addressed verified resource cache
```

The resolver retains the `FontCatalogGeneration` shared pointer for the complete synchronous call, so locator string views remain valid even if another generation is published concurrently.

## Supported portable capabilities

### Fontconfig

A Fontconfig identity is directly resolved only when:

- exactly one file path is present;
- an explicit face index is present;
- the sysroot field is empty.

A non-empty sysroot is rejected. This slice does not guess whether `FC_FILE` is already rooted or how an external Fontconfig sysroot should be joined.

### DirectWrite

A DirectWrite identity is directly resolved only when it contains exactly one local file and an explicit face index.

Multi-file faces are structurally recognized by Z2B-8 but rejected by this portable resolver. A later native provider may expose a stable byte stream for those faces.

### CoreText

The current CoreText discovery identity contains a file path and PostScript name but no sfnt face index. The resolver therefore rejects it as `FaceIndexUnresolved`; it never assumes face zero.

## Path conversion

Discovery paths are UTF-8. The resolver converts the bounded UTF-8 field to a C++20 `std::filesystem::path` through `std::u8string`, preserving Unicode behavior on Windows and byte behavior on POSIX systems.

The portable path field is limited to 32,768 UTF-8 bytes before conversion.

## Error chain

The resolver keeps every stage visible:

- invalid generation or face id;
- platform identity parse error, byte offset, and field index;
- unsupported sysroot, multi-file, or unresolved-face capability;
- path length or conversion failure;
- file metadata/open/read/stability error;
- content identity/cache error;
- verified-resource parse or integrity error.

The complete file-loader chain remains available in `CatalogFontResourceError::file_error`.

## Failure atomicity

The caller output is reset before validation. No handle is published until:

1. the generation and face id are valid;
2. the identity parses;
3. the capability is portable-loadable;
4. UTF-8 path conversion succeeds;
5. exact bounded file loading succeeds;
6. content identity and cache publication succeed;
7. SFNT/TTC and integrity verification succeed.

## Certified behavior

The focused suite covers:

- Fontconfig catalog face resolution;
- single-file DirectWrite catalog face resolution;
- two different platform identities for the same file converging on the identical immutable resource handle;
- generation id, face id, identity bytes, path bytes, locator, staging, and cache statistics;
- non-empty Fontconfig sysroot rejection before I/O;
- DirectWrite multi-file rejection before I/O;
- CoreText unresolved face-index rejection;
- malformed identity error chaining;
- 32,768-byte portable path limit;
- missing-file nested error propagation;
- invalid face-id rejection;
- failure clearing a previously populated output;
- strict GCC, AppleClang, and MSVC builds;
- Linux and macOS ASan/UBSan.

## Explicit boundary

This slice does not join Fontconfig sysroots, provide native DirectWrite multi-file streams, derive CoreText face indices, memoize path metadata, memory-map files, cache HarfBuzz objects, rasterize, or paint.
