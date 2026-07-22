# M2 Dynamic Height Index

The M2 viewport index keeps source payload bytes immutable while allowing measured layout heights to replace initial estimates.

## Data structures

- `record-heights.idx`: one 32-bit Q8 height per logical record;
- `height-blocks.idx`: one 64-bit aggregate per bounded record block;
- in-process Fenwick tree: O(log block-count) prefix lookup and aggregate replacement;
- `arena.meta`: persisted total logical height and immutable arena configuration.

The Fenwick tree scales with block count, not record count. With 8,192 records per block, 8,388,608 records require only 1,024 block values.

## Update transaction

`height-update` performs bounded writes in this order:

1. validate record index, new height, block underflow/overflow, and total-height overflow;
2. write the record height;
3. write the affected block aggregate;
4. write the arena total-height header;
5. update the in-process Fenwick tree.

If a later metadata write fails, earlier metadata writes are restored where possible. The source payload and store integrity hashes are never modified. The arena is rebuildable from `records.idx` if metadata recovery is required.

## Required invariants

- source payload bytes remain unchanged;
- logical IDs and record ordering remain unchanged;
- every nonzero record height belongs to exactly one block aggregate;
- the sum of all block aggregates equals `arena.meta.total_height_q8`;
- viewport offsets after reopen use persisted corrected heights;
- memory use remains bounded by block size plus the small Fenwick tree.

## Current boundary

This milestone supports height correction, not record insertion/removal. Dynamic sequence edits require the next order-statistics sequence layer and crash-safe arena journaling.
