# M2 — Chunked Order-Statistics Sequence

This line adds the mutable document-order layer required by the M2 mainline plan. Sequence correctness, snapshot isolation, runtime arena integration and later durability work are admitted separately so a persistence defect cannot hide inside data-structure certification.

## Representation

`ChunkedOrderStatisticsSequence` stores logical records in bounded struct-of-arrays chunks. A record carries:

- stable logical ID;
- source/text byte count;
- Q8 layout height;
- a 64-bit search-summary bitset.

Chunks are leaves embedded in an AVL order tree. The default chunk capacity is 256 records and the constructor clamps the configured capacity to 8..4096 records. There is no heap object per logical record.

Every tree node stores subtree aggregates for:

- record count;
- text bytes;
- layout height;
- search-summary OR;
- chunk count and AVL height.

## Persistent copy-on-write roots

The current root, child links and chunks are immutable shared objects. `snapshot()` captures the current root state with a shared ownership increment only; it does not traverse or clone the tree.

A mutation builds a replacement root transactionally:

- unchanged subtrees and unchanged chunks are shared;
- only the changed chunk is copied;
- only the `O(log C)` AVL path and any rotation/split nodes are recreated;
- the live sequence publishes the replacement root only after the full mutation succeeds;
- snapshots retain the old root and therefore remain immutable while the live sequence continues to mutate.

This gives O(1) snapshot creation and permits readers to retain immutable roots without observing concurrent writer state transitions. Normal mutation does not perform a full-tree clone or rebuild.

## Compact arena runtime integration

`CompactArenaReader::open()` now validates the persistent arena indices and builds the live logical-order root from the exact `records.idx` descriptors plus `arena/record-heights.idx` values. Each persisted height block is cross-checked against the records admitted into the sequence before the reader becomes usable.

Viewport materialization no longer uses the block Fenwick table as its document-order/select authority. It captures one immutable logical snapshot, resolves the first visible ordinal with `locate_height_offset()`, and obtains subsequent record identity, source-byte count, height and Y prefix from that same root. The persisted block table remains only as a compact durability/checking structure for the current height file.

`logical_snapshot()` exposes the immutable root to higher-level browser/layout readers without copying the document-order structure. Height correction checks that the persisted height and logical root agree before mutation, persists the existing arena files, then publishes the new copy-on-write root. A failed root publication attempts to roll the persisted files back to the previous values.

This integration intentionally keeps logical ordinal equal to the current physical source-record index. Reorder/insert/delete are therefore not yet exposed through `CompactArenaReader`: that requires an explicit stable physical-source locator so a moved logical record can still read the correct immutable MassiveDoc payload.

## Operations and bounds

Let `C` be the number of chunks and `B` the configured bounded chunk capacity.

- record access/rank select: `O(log C + B)`;
- text-offset lookup: `O(log C + B)`;
- layout-height lookup: `O(log C + B)`;
- prefix aggregate: `O(log C + B)` for the boundary chunk;
- insert/erase/move: `O(log C + B)` plus bounded chunk split/shift work;
- height/search-summary update: `O(log C + B)`;
- snapshot creation/copy: `O(1)`.

Because `B` is a fixed configuration bound, normal document-order operations are logarithmic in sequence size.

## Mutation semantics

- insert accepts positions in `[0, size]` and rejects zero layout height;
- erase returns the exact removed record;
- move uses final-index semantics: after removing the source record, it is inserted at the requested index in the resulting sequence;
- height updates preserve record identity and update all affected subtree height aggregates;
- search-summary updates preserve record identity and rebuild affected summary OR aggregates;
- invalid indices and aggregate overflow fail closed;
- a failed persistent mutation leaves the previously published root intact.

This layer owns only logical sequence metadata. It does not rewrite MassiveDoc source payload segments or source integrity hashes.

## Validation

The compact-document test target runs a deterministic sequence oracle that covers:

- chunk splitting and AVL height bounds;
- record-rank access and exact text/height prefixes;
- prefix aggregate correctness;
- 5,000 deterministic mixed insert/erase/move/height/search-summary mutations against a `std::vector` oracle;
- snapshot isolation before and after multiple live mutations;
- copied snapshots retaining immutable aggregate and record state;
- 10,000 O(1)-shape snapshot handle copies;
- a 100,000-record append scale case with tail height-select and bounded chunk/tree-height assertions;
- compact-arena root reconstruction from persisted descriptors/heights;
- viewport start selection and Y prefixes coming from the immutable sequence root;
- reader-level snapshot isolation across persisted height corrections;
- reopened roots matching the persisted corrected height aggregate.

Cross-platform repository CI remains the admission authority. Local strict-warning and sanitizer runs are supporting evidence only.

## Remaining M2 boundary

This line still does **not** claim M2 complete. Still required:

1. split logical ordinal from immutable physical source-record identity and thread that locator through layout, checkpoint and cache consumers before arena-level insert/erase/move can be admitted;
2. remove any remaining browser document-order vector/O(n) position-map ownership in favor of the shared sequence root;
3. persistence/durability integration for logical-order mutations. Crash-safe generation manifests and append journaling remain a later storage-hardening boundary and are not credited by the in-memory sequence/snapshot/runtime-integration passes.
