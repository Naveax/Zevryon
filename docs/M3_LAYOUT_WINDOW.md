# M3A Bounded Incremental Layout Window

M3A connects the disk-backed MassiveDoc store and M2 compact height arena to a bounded line-fragment working set.

## Pipeline

```text
scroll position
  -> Fenwick-backed CompactArenaReader viewport
  -> streaming StoreReader record scan
  -> deterministic UTF-8 byte-range line fragments
  -> measured record-height update
  -> proportional same-record scroll-anchor correction
  -> corrected viewport rematerialization
  -> conservatively charged byte-budgeted LRU fragment cache
```

The complete payload, complete record descriptor set, and complete fragment set are never required to be resident at once.

## Memory contract

The fragment cache is governed by a byte budget, not record count. The default CLI budget is 8 decimal MB.

Resident charge includes:

- `LayoutFragment` vector capacity rather than logical size;
- cache-entry and cache-key metadata;
- a conservative allowance for map/list nodes and allocator bookkeeping.

A cache entry whose complete fragment vector cannot fit releases that vector and retains only bounded measurement metadata. Repeated giant-record viewports can therefore reuse measured height without pretending that all line fragments are resident.

The corrected viewport pass reads complete cached fragment vectors by reference; it does not bulk-copy the full cached vector. Only fragments that survive viewport intersection are copied into the bounded result.

`max-fragments` independently caps the visual result. A 16 MiB unbroken token therefore cannot allocate an unbounded visible fragment vector or cache entry.

## Scroll correction

M3A records the logical record intersecting the requested scroll position before measured heights replace estimates. After reflow it preserves the same proportional position inside that record:

```text
new local offset = old local offset * new record height / old record height
```

Height deltas from measured records before the anchor are applied to the anchor record's new document position. The corrected scroll is then clamped to the valid document end.

JSON exposes two separate signals:

- `scroll_anchor_adjusted`: the same-record proportional position changed;
- `scroll_clamped`: the corrected anchor still exceeded the final document boundary.

This prevents a large record from disappearing from the viewport when byte-based estimated height contracts after codepoint-based layout. It is intentionally bounded same-record anchoring, not full browser scroll anchoring. DOM node selection, line-level anchors, mutation suppression rules, nested scrollers, and CSSOM integration remain later work.

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
- cache hits, misses, and conservatively charged bytes;
- anchor-adjustment, end-clamp, truncation, and height-saturation flags;
- bounded fragments with logical identity and source byte ranges.

## Acceptance gates

- strict C++20 build and native tests on Linux, macOS, and Windows;
- Linux ASan/UBSan;
- MSVC AddressSanitizer for store, compact-arena, and layout-window tests;
- deterministic newlines and UTF-8 window boundaries;
- conservative cache accounting and cache reuse under the configured byte cap;
- metadata-only reuse for an oversized fragment set;
- proportional same-record anchor retention across height contraction;
- 16 MiB unbroken-token test;
- existing 128 MiB corpus with a real 64 MiB record remains lossless;
- post-layout CRC32 and full-payload SHA-256 verification.

## Current boundary

M3A produces deterministic line fragments using average advance metrics and bounded same-record proportional anchoring. Font shaping, CSS inline formatting, line/DOM-level scroll anchoring, sparse giant-record line checkpoints, text interaction, paint, and accessibility projection are separate subsequent milestones.
