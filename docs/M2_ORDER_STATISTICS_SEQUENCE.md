# M2 — Chunked Order-Statistics Sequence

This pass adds the first mutable document-order layer required by the M2 mainline plan. It is intentionally separate from persistence and crash-recovery work so sequence correctness can be admitted independently from durability behavior.

## Representation

`ChunkedOrderStatisticsSequence` stores logical records in bounded struct-of-arrays chunks. A record carries:

- stable logical ID;
- source/text byte count;
- Q8 layout height;
- a 64-bit search-summary bitset.

Chunks are nodes in an AVL tree. The default chunk capacity is 256 records and the constructor clamps the configured capacity to 8..4096 records. There is no heap object per logical record.

Every tree node stores subtree aggregates for:

- record count;
- text bytes;
- layout height;
- search-summary OR;
- chunk count and AVL height.

## Operations and bounds

Let `C` be the number of chunks and `B` the configured bounded chunk capacity.

- record access/rank select: `O(log C + B)`;
- text-offset lookup: `O(log C + B)`;
- layout-height lookup: `O(log C + B)`;
- prefix aggregate: `O(log C + B)` for the boundary chunk;
- insert/erase/move: `O(log C + B)` plus bounded chunk split/shift work;
- height/search-summary update: `O(log C + B)`.

Because `B` is a fixed configuration bound, normal document-order operations are logarithmic in sequence size and do not rebuild the full tree.

## Mutation semantics

- insert accepts positions in `[0, size]` and rejects zero layout height;
- erase returns the exact removed record;
- move uses final-index semantics: after removing the source record, it is inserted at the requested index in the resulting sequence;
- height updates preserve record identity and update all affected subtree height aggregates;
- search-summary updates preserve record identity and rebuild affected summary OR aggregates;
- invalid indices and aggregate overflow fail closed.

This layer owns only logical sequence metadata. It does not rewrite MassiveDoc source payload segments or source integrity hashes.

## Validation in this pass

The compact-document test target also runs a deterministic sequence oracle that covers:

- chunk splitting and AVL height bounds;
- record-rank access and exact text/height prefixes;
- prefix aggregate correctness;
- 5,000 deterministic mixed insert/erase/move/height/search-summary mutations against a `std::vector` oracle;
- a 100,000-record append scale case with tail height-select and bounded chunk/tree-height assertions.

Cross-platform repository CI remains the admission authority. Local developer runs are supporting evidence only.

## Remaining M2 boundary

This pass does **not** claim M2 complete. Still required:

1. copy-on-write roots for immutable snapshots and concurrent readers;
2. integration of this sequence with browser logical-node ownership and compact arena materialization;
3. removal of remaining document-order vector/O(n) position-map consumers;
4. persistence/durability integration. Crash-safe generation manifests and append journaling remain a later storage-hardening boundary and are not credited by this in-memory sequence pass.
