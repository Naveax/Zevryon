# M2 — Compact Arena and Bounded Viewport Materialization

M2 connects the disk-backed M1 store to a document geometry layer without allocating one C++ object per logical node.

## Persistent representation

- `arena/record-heights.idx`: one 32-bit Q8 height estimate per logical record;
- `arena/height-blocks.idx`: one 64-bit aggregate height per 8,192 records;
- `arena/arena.meta`: fixed-size versioned metadata.

The 67,108,864-node certification count remains virtual and exact in metadata. It is not expanded into 67 million resident objects. Source bytes remain in the segmented MassiveDoc store.

## Viewport query

1. Binary-search the small block-prefix table.
2. Read one height block from disk.
3. Batch-read the matching record descriptors.
4. Materialize only records intersecting viewport plus overscan.
5. Stop at a hard record cap selected by the process-group pressure policy.

## 64 MiB evidence

- 131,072 records / 1,048,576 logical nodes;
- arena physical size: 524,488 bytes;
- arena build peak PSS: 1.104896 MB;
- top/middle/end viewport count: 12 / 18 / 12;
- engine viewport time: 0.285–0.569 ms;
- final record reachable without scanning from record zero;
- source SHA-256 unchanged.

These are native core measurements on the validation host, not whole-browser or mobile-device results.
