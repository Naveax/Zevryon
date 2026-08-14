# M2 — Session Logical Move and Physical Height Persistence

This pass opens the first compact-arena logical reorder boundary without pretending that logical-order durability already exists.

## Identity split

`CompactArenaReader::move_logical_record()` changes only the live persistent order-statistics root. It does not rewrite `records.idx`, payload segments, `record-heights.idx`, height-block indices or any generation manifest.

Therefore:

- the move is **session-only / non-durable**;
- snapshots taken before the move remain immutable;
- reopening the arena reconstructs the original physical record order;
- each moved record retains its immutable `source_record_index`.

The method is intentionally narrow. Insert/erase are still not exposed at the arena boundary.

## Height persistence authority

Height correction accepts a logical ordinal because layout/scroll code addresses the live sequence in logical order. Before touching disk, `update_height()` resolves that logical ordinal through the current sequence root and obtains the record's immutable `source_record_index`.

Persistent height state remains physically keyed:

- `record-heights.idx` offset = `source_record_index * entry_size`;
- `height-blocks.idx` block = `source_record_index / records_per_block`;
- block Fenwick bookkeeping remains physical-source keyed;
- the global total height remains valid because summation is independent of logical order;
- the copy-on-write sequence update still targets the caller's logical ordinal.

This prevents a moved logical record from overwriting another physical record's persisted height.

## Behavioral oracle

The compact-document test performs the divergence that earlier consumers could not yet create through the arena API:

1. persist a corrected height for physical source record `0`;
2. snapshot the current root;
3. move logical ordinal `0` to logical ordinal `2`;
4. verify the moved record is now logical `2` but still physical source `0`;
5. materialize at the moved record's Y coordinate and verify both identities;
6. update height through logical ordinal `2`;
7. require the reported physical height block to remain source block `0`;
8. verify the live moved record receives the new height;
9. reopen the arena and require logical order to reset to physical order while source record `0` retains the newly persisted height;
10. require the pre-move snapshot to remain unchanged.

This is the first end-to-end proof inside `CompactArenaReader` that mutable order and immutable storage identity are genuinely separate rather than merely two fields carrying the same number.

## Remaining admission boundary

This pass does **not** make reorder durable and does not claim M2 complete. Next work remains:

1. expose narrowly scoped session move forwarding in the higher-level layout/hot-scroll engines;
2. run moved-record end-to-end oracles proving ordinary layout, checkpoint-aware layout and hot-scroll read physical payload/checkpoints/raw windows while emitting logical order;
3. audit the remaining repository for any logical-ordinal source dereference;
4. design crash-safe logical-order journaling/generation persistence;
5. only after durable recovery is certified may logical moves survive reopen and receive full M2 persistence credit.
