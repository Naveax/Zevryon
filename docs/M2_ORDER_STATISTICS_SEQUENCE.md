# M2 — Chunked Order-Statistics Sequence

This line provides Zevryon's mutable document-order authority: a bounded-chunk persistent order-statistics sequence with immutable physical source identity, copy-on-write snapshots, compact-arena runtime ownership, and durable logical-move recovery.

## Representation

`ChunkedOrderStatisticsSequence` stores logical records in bounded struct-of-arrays chunks. A record carries:

- stable logical ID;
- source/text byte count;
- Q8 layout height;
- a 64-bit search-summary bitset;
- immutable physical `source_record_index` identity.

The physical source locator is deliberately independent from current logical ordinal. Insert, erase, move, chunk split/rotation, root fork and snapshots preserve it as record identity.

Chunks are leaves in an AVL order tree. The default chunk capacity is 256 records and configured capacity is bounded to 8..4096 records. There is no heap object per logical record.

Every subtree stores aggregate record count, text bytes, layout height, search-summary OR, chunk count and AVL height.

## Persistent copy-on-write roots

Roots, child links and chunks are immutable shared objects. `snapshot()` is O(1): it captures the current root with shared ownership and does not traverse or clone the tree.

A mutation creates a replacement root transactionally:

- unchanged subtrees/chunks are shared;
- the changed bounded chunk is copied;
- only the affected `O(log C)` AVL path and split/rotation nodes are recreated;
- the live sequence publishes the replacement root only after mutation succeeds;
- old snapshots continue to reference the prior immutable root.

`fork_shared_root()` creates an O(1) mutable candidate sharing the same immutable root. Durable arena moves mutate this candidate first, commit its order generation to disk, then replace the live sequence by move-assignment only after durable verification.

## Compact arena authority

`CompactArenaReader::open()` validates the physical arena indices, reconstructs physical records from `records.idx` plus `record-heights.idx`, then loads the latest committed logical-order generation. The persisted permutation is applied to the physical snapshot to reconstruct the live logical root.

If no logical-order generation exists, older arenas open in identity order as generation `0`. Torn temporary candidates are ignored. Invalid committed generations fail open closed.

Viewport materialization captures one immutable logical snapshot, resolves the first visible ordinal with `locate_height_offset()`, and exposes:

- current logical `record_index`;
- immutable `source_record_index`;
- logical Y prefix and height from the same root.

`logical_snapshot()` exposes the immutable root to higher-level readers without copying the sequence.

## Logical move durability

`CompactArenaReader::move_logical_record()` is a durable operation:

1. fork the live COW root in O(1);
2. apply the logical move to the candidate;
3. serialize the candidate's physical-source permutation;
4. publish generation `current + 1` through the durable logical-order publisher;
5. reload and verify the committed generation;
6. publish the candidate as the new live root.

A same-index move is a no-op and creates no generation. A publication failure closes the reader rather than continuing from a potentially ambiguous commit outcome.

The durable generation file is `logical-order.g<16-hex-generation>.zmd`; committed generations are immutable and older generations are not overwritten.

## Physical source and height persistence

Logical reorder never rewrites source payload segments or redefines physical source identity.

Height correction accepts the current logical ordinal, resolves its `source_record_index`, and persists to the physical height slot/block. Thus a moved record may be logical ordinal `N` while its payload, checkpoint and persisted height remain owned by physical source `P`.

The total height aggregate is order-independent, so physical block accounting remains valid across logical permutation.

## Operations and bounds

Let `C` be chunk count and `B` the configured bounded chunk capacity.

- record access/rank select: `O(log C + B)`;
- text-offset lookup: `O(log C + B)`;
- layout-height lookup: `O(log C + B)`;
- prefix aggregate: `O(log C + B)`;
- insert/erase/move: `O(log C + B)` plus bounded split/shift work;
- height/search-summary update: `O(log C + B)`;
- snapshot creation: `O(1)`;
- candidate root fork: `O(1)`.

Because `B` is fixed and bounded, normal in-memory order operations are logarithmic in sequence size. Persisting a move serializes the order permutation and is therefore intentionally separate from the in-memory mutation bound.

## Validation

The compact-document test authority covers:

- AVL/chunk bounds and rank/prefix correctness;
- deterministic mixed insert/erase/move/height/search-summary mutation against a vector oracle;
- large append/select cases;
- snapshot isolation and O(1)-shape snapshot copies;
- immutable source locator through move/erase/snapshots;
- O(1) shared-root fork isolation;
- compact-arena sequence ownership and materialization;
- logical/physical source divergence;
- physical height correction after logical move;
- durable generation publication and reopen recovery;
- multiple generations and same-index no-op semantics;
- torn temporary generation recovery;
- corrupt committed generation fail-closed behavior;
- reopened LayoutWindow and HotScroll consumers retaining correct logical order and physical payload/checkpoint identity.

Cross-platform exact-head Windows/Linux CI is the admission authority.

## Remaining M2 boundary

The durable move primitive and its main runtime consumers are implemented. Remaining closure work is repository-level:

1. complete the residual audit for logical-ordinal physical-source dereferences;
2. keep compact-arena insert/erase closed until they receive their own durable storage protocol;
3. verify the final branch diff against fresh `main` and exact-head CI;
4. produce M2 promotion/evidence receipts only after those closure gates pass.
