# M2 — Chunked Order-Statistics Sequence

This line adds the mutable document-order layer required by the M2 mainline plan. Sequence correctness, snapshot isolation, runtime arena integration, source identity and later durability work are admitted separately so a persistence defect cannot hide inside data-structure certification.

## Representation

`ChunkedOrderStatisticsSequence` stores logical records in bounded struct-of-arrays chunks. A record carries:

- stable logical ID;
- source/text byte count;
- Q8 layout height;
- a 64-bit search-summary bitset;
- immutable physical `source_record_index` identity.

The source locator is deliberately independent from the current logical ordinal. Insert, erase, move, chunk split/rotation and copy-on-write snapshots preserve it as record identity; subtree aggregates do not reinterpret it as an ordinal.

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

`CompactArenaReader::open()` validates the persistent arena indices and builds the live logical-order root from the exact `records.idx` descriptors plus `arena/record-heights.idx` values. Each persisted height block is cross-checked against the records admitted into the sequence before the reader becomes usable.

Every admitted arena record now stores its immutable physical `records.idx` ordinal in `source_record_index`. Initial construction has `logical ordinal == source_record_index`, but the two values are distinct fields so later logical moves do not need to redefine payload identity.

Viewport materialization captures one immutable logical snapshot, resolves the first visible ordinal with `locate_height_offset()`, and exposes both the current logical `record_index` and immutable `source_record_index` in `MaterializedRecord`. Source-byte count, height and Y prefix still come from that same root. The persisted block table remains only as a compact durability/checking structure for the current height file.

`logical_snapshot()` exposes the immutable root to higher-level browser/layout readers without copying the document-order structure. Height correction checks that the persisted height and logical root agree before mutation, persists the existing arena files, then publishes the new copy-on-write root. A failed root publication attempts to roll the persisted files back to the previous values.

The physical locator is now populated and visible at the materialization boundary. Arena-level reorder/insert/delete remains closed until layout, checkpoint and source/cache consumers are proven to dereference `source_record_index` rather than mutable logical ordinal.

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
- erase returns the exact removed record, including physical source identity;
- move uses final-index semantics and preserves `source_record_index` while changing only logical order;
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
- immutable source locator surviving move, erase and a pre-mutation snapshot;
- compact-arena root reconstruction assigning exact physical source ordinals;
- materialization exposing the same source locator as the selected sequence record;
- viewport start selection and Y prefixes coming from the immutable sequence root;
- reader-level snapshot isolation across persisted height corrections;
- reopened roots matching the persisted corrected height aggregate.

Cross-platform repository CI remains the admission authority. Local strict-warning and sanitizer runs are supporting evidence only.

## Remaining M2 boundary

This line still does **not** claim M2 complete. Still required:

1. thread `source_record_index` through layout payload reads, layout-checkpoint identity/pathing, hot-scroll source-window keys and related caches while keeping logical ordinals for anchors/order;
2. only after that consumer split is verified, admit arena-level reorder/move semantics without risking reads from the wrong source payload;
3. remove any remaining browser document-order vector/O(n) position-map ownership in favor of the shared sequence root;
4. persistence/durability integration for logical-order mutations. Crash-safe generation manifests and append journaling remain a later storage-hardening boundary and are not credited by the in-memory sequence/snapshot/runtime-integration passes.
