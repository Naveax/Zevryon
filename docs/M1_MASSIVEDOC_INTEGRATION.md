# M1 — Native MassiveDoc Store and Document-Process Integration

## Delivered

- `massivedoc_store.hpp/.cpp`: streaming segmented store.
- `zevryon-massivedoc`: import, stats, search, verify, export CLI.
- Browser protocol commands for store open/search/bounded record slices.
- Decimal-MB process-group targets and hard caps.
- Four-level pressure controller: normal, soft, hard, survival.
- Native roundtrip, giant-record, and corruption tests.
- Reproducible Linux PSS benchmark harness.

## File layout

```text
store/
├── manifest.zmd
├── records.idx
├── chunks.idx
├── search.bgm
└── segments/
    ├── segment-00000000.bin
    └── ...
```

A record descriptor contains logical identity, first chunk, length, chunk count, and CRC32. A chunk descriptor maps a record range to a segment, offset, and length. Records may cross segment boundaries.

Search blocks cover 8,192 records by default. Every block stores a direct 65,536-bit bigram signature. The signature can produce false positives but cannot reject a block containing every query bigram. Exact byte verification is always performed on candidate payloads.

## Current measured smoke result

The checked-in evidence uses 64 MiB payload and 131,072 records:

- import peak PSS: about 3.05 MB;
- cold final-record marker search: about 19.7 ms engine time, 2.93 MB peak PSS;
- full CRC32 + SHA-256 verification: about 1.97 MB peak PSS;
- payload hash match and zero data loss.

These numbers apply to the native store process, not the complete browser process group.

## Next exit gate

- compact struct-of-arrays logical node arena;
- order-statistics sequence/height index;
- viewport materializer using bounded record slices;
- process-group PSS telemetry wired into browser supervision;
- 4 GiB interactive first-viewport test under 96 MB, followed by 80 MB and 64 MB profiles.
