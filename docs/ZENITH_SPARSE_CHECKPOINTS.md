# ZENITH Sparse Layout Checkpoints

ZENITH sparse checkpoints remove the full-record scan from random viewport access while preserving the M3A immutable-payload contract.

## Scope

The checkpoint index is derived, rebuildable metadata. It never changes:

- source payload bytes;
- logical IDs or record ordering;
- record CRC32 values;
- full-payload SHA-256;
- record and chunk descriptors.

A missing, stale, or corrupt checkpoint causes the checkpoint-aware global layout entry to decline acceleration. The caller can then use the existing M3A full-scan path without changing page semantics.

## Persistent format

Each checkpoint file contains:

- a 96-byte versioned header;
- record identity and source length;
- layout metric configuration;
- measured record height;
- physical-size metadata;
- a checksum over the entry table;
- 24-byte monotonic entries containing source offset, logical line, and local Y.

The default 64 KiB stride creates 1,024 entries for the current 64 MiB giant-record corpus. The resulting file is 24,672 bytes, or 0.036764% of the source record.

## Query pipeline

```text
Global scroll Y
  -> Fenwick record lookup
  -> persistent checkpoint validation
  -> nearest checkpoint by local Y
  -> bounded record-slice reads
  -> visible fragments
  -> global Y projection
```

The standalone checkpoint command and the global checkpoint-aware layout API both retain the existing fragment and source-range contracts.

## Reproducible 64 MiB evidence

GitHub-hosted Ubuntu evidence from the same 128 MiB corpus and runner:

| Metric | M3A full scan | ZENITH checkpoint |
|---|---:|---:|
| Random viewport P50 | 311.404 ms single baseline | 2.761 ms across 21 positions |
| Random viewport P95 | 311.404 ms single baseline | 2.945 ms |
| Random viewport P99 | 311.404 ms single baseline | 2.998 ms |
| Worst query | 311.404 ms | 3.012 ms |
| Source bytes read | 134,217,728 | at most 131,072 |
| Peak process PSS | 5.366 MB | at most 0.182 MB sampled |
| Persistent index | none | 24,672 bytes |
| Data loss | 0 | 0 |

Derived results:

- P50 speedup: 112.77x;
- P95 speedup: 105.75x;
- worst-case source-read reduction: 1,024x;
- one-time checkpoint build: 154.64 ms;
- build amortization: less than one avoided full-scan query.

The checkpoint timings include process start, store open, checkpoint validation, bounded reads, fragment production, and JSON serialization.

## Browser comparison laboratory

The competitor workflow used a 64 MiB payload and 21 random queries on the same GitHub-hosted runner. Each browser was measured in two modes:

1. `virtualized`: a 64 MiB Blob remains owned by the browser and only a bounded 128 KiB slice is decoded and rendered per query;
2. `native-dom`: the complete 64 MiB payload is decoded into one browser text node and laid out by the browser.

| Engine and mode | Query P50 | Query P95 | Incremental peak process-tree PSS | Setup/result |
|---|---:|---:|---:|---|
| Zevryon checkpoint | 2.761 ms | 2.945 ms | 0.182 MB sampled process peak | success |
| Chromium 149 virtualized | 82.7 ms | 103.2 ms | 426.09 MB | success |
| Firefox 151 virtualized | 30.0 ms | 32.0 ms | 494.50 MB | success |
| Chromium 149 native DOM | — | — | — | timed out after 420 s |
| Firefox 151 native DOM | 30.0 ms | 31.0 ms | 1,131.26 MB | 4.483 s setup, success |

## Interpretation boundary

This comparison proves a viewport/storage/indexing advantage, not complete browser rendering superiority.

- Zevryon currently emits deterministic average-advance fragments.
- Chromium and Firefox native DOM perform real browser text layout.
- Browser virtualized query timings are measured in an already-running page.
- Zevryon timings include a new CLI process and index/store opening on every query.
- Browser memory includes the browser process tree above the Python/Playwright harness baseline.
- Zevryon memory is the measured native query process PSS.

The next correctness milestone is real shaping, grapheme clustering, bidi, font fallback, and Unicode line breaking over the same bounded checkpoint path.

## Acceptance gates

- strict C++20 builds on Linux, macOS, and Windows;
- Linux ASan/UBSan;
- MSVC AddressSanitizer for store, arena, M3A layout, checkpoint, and global ZENITH layout tests;
- corrupt checkpoint rejection;
- safe legacy fallback when checkpoints are unavailable;
- 21-position 64 MiB random access distribution;
- no query above 2 MiB source reads;
- checkpoint overhead below 0.5%;
- post-layout full payload verification.
