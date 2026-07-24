# Z2B-7 — Bounded Stable Font File Loader

## Purpose

`load_verified_font_file` is the portable file boundary between a platform-discovered path and the verified, content-addressed font resource cache.

It ensures that a file is read under an explicit staging budget, that detectable metadata changes during the read are rejected, and that only the exact bytes actually read can become a cache identity or immutable verified resource.

## Load sequence

The loader executes this order:

1. validate path, cache, output, stats, error, and non-zero staging limit;
2. require a regular file;
3. read preflight file size and write time;
4. reject empty, oversized, `size_t`-overflowing, or `streamsize`-overflowing input;
5. allocate one PMR staging buffer under `FontFileReadBuffer`;
6. read exactly the preflight byte count;
7. attempt one additional byte read to detect growth;
8. re-read file size and write time;
9. reject any detectable metadata change;
10. compute the domain-separated content identity;
11. publish through the bounded single-flight verified-resource cache;
12. destroy the staging buffer before returning;
13. require staging ledger current bytes to be zero before publishing output.

## Resource accounting

The temporary file copy is accounted separately from retained font bytes:

- `FontFileReadBuffer`: transient exact-read staging bytes;
- `FontResourceBytes`: immutable bytes owned by the verified resource;
- `FontResourceCacheRetention`: cache ownership of retained resources.

A successful call returns with `FontFileReadBuffer.current_bytes == 0`. Peak and physical-read counters remain available in `FontFileLoadStats`.

## Stability and TOCTOU boundary

The portable layer compares file size and `last_write_time` before and after the exact read. It also checks for an unexpected extra byte.

These checks reject detectable truncation, growth, and timestamp-changing replacement. Portable C++ cannot promise inode/device identity or no-follow semantics on every platform. Therefore:

- the path itself is not trusted as content identity;
- cache identity is derived only from bytes actually read;
- SFNT/TTC structure and integrity are verified after reading;
- a same-size, same-timestamp replacement cannot substitute another cached resource because the content identity changes and cache collision defense remains active.

Platform-specific no-follow or handle-identity hardening may be layered above this portable boundary later.

## Failure atomicity

Every failure clears the caller output. No cache publication occurs for:

- missing or unreadable metadata;
- non-regular paths;
- empty files;
- staging-limit rejection;
- stream-size overflow;
- staging allocation failure;
- open or exact-read failure;
- detectable file mutation during read;
- content identity failure;
- SFNT/TTC parse failure;
- integrity failure;
- cache publication failure.

Nested cache, resource, parser, integrity, byte offset, and table-tag errors remain available through `FontFileLoadError::cache_error`.

## Certified behavior

The focused suite covers:

- valid exact-limit load;
- exact byte and physical-read accounting;
- staging buffer release before return;
- stable metadata recording;
- repeated load returning the identical immutable handle;
- missing path rejection;
- directory rejection;
- empty-file rejection;
- pre-read oversize rejection;
- checksum-corrupt font rejection with full error chain;
- invalid face-index rejection with parser detail;
- failure clearing a previously populated output;
- strict GCC, AppleClang, and MSVC builds;
- Linux and macOS ASan/UBSan.

## Explicit boundary

This slice does not enumerate platform fonts, prevent symlink traversal portably, persist path-to-content mappings, memory-map files, cache file metadata, perform asynchronous I/O, cache HarfBuzz objects, rasterize, or paint.
