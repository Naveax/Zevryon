# M3A Bounded Incremental Layout Window

M3A connects the disk-backed MassiveDoc store and M2 compact height arena to a bounded line-fragment working set.

## Pipeline

```text
scroll position
  -> Fenwick-backed CompactArenaReader viewport
  -> streaming StoreReader record scan
  -> deterministic UTF-8 byte-range line fragments
  -> measured record-height update
  -> corrected viewport rematerialization
  -> byte-budgeted LRU fragment cache
```

The complete payload, complete record descriptor set, and complete fragment set are never required to be resident at once.

## Memory contract

The fragment cache is governed by a byte budget, not record count. The default CLI budget is 8 decimal MB. A cache entry that cannot fit within the configured budget retains only bounded measurement metadata; its complete fragments are not cached.

`max-fragments` independently caps the visual result. A 16 MiB unbroken token therefore cannot allocate an unbounded visible fragment vector or cache entry.

## Source and integrity boundary

M3A does not change:

- payload bytes;
- logical IDs or record ordering;
- per-record CRC32;
- full-payload SHA-256;
- record and chunk descriptors.

Measured heights update only the rebuildable arena metadata introduced in M2.

## UTF-8 behavior

The scanner carries incomplete UTF-8 sequences across the store's 64 KiB I/O windows. Fragment source ranges therefore never split the byte sequence currently being consumed. Invalid or incomplete sequences receive deterministic replacement-width treatment without rewriting source bytes.

M3A is not a shaping engine. It does not yet implement grapheme clustering, bidi reordering, script shaping, font fallback, CSS white-space processing, or Unicode line-breaking classes.

## CLI

```bash
build/zevryon-massivedoc layout-window \
  <store-dir> <scroll-y-px> <width-px> <height-px> \
  [overscan-px] [max-fragments] [cache-mb]
```

Example:

```bash
build/zevryon-massivedoc layout-window store 0 800 720 360 512 8
```

The command returns JSON containing:

- corrected query and total-height values;
- source bytes read;
- measured record count;
- cache hits, misses, and charged bytes;
- truncation and height-saturation flags;
- bounded fragments with logical identity and source byte ranges.

## Acceptance gates

- strict C++20 build on Linux, macOS, and Windows;
- Linux ASan/UBSan;
- MSVC AddressSanitizer for store, compact-arena, and layout-window tests;
- deterministic newlines and UTF-8 window boundaries;
- cache reuse under its configured byte cap;
- 16 MiB unbroken-token test;
- existing 128 MiB corpus with a real 64 MiB record remains lossless.

## Current boundary

M3A produces deterministic line fragments using average advance metrics. Scroll-anchor preservation, font shaping, CSS inline formatting, text interaction, paint, and accessibility projection are separate subsequent milestones.
