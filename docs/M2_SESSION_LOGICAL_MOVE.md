# M2 — Durable Logical Move and Physical Source Persistence

M2 logical reorder is now a durable, copy-on-write order-statistics path. Logical order may diverge from physical storage order without rewriting payload segments, `records.idx`, `record-heights.idx`, or physical height-block indices.

## Identity split

Every live sequence record carries two independent identities:

- `record_index` is the current logical ordinal in the persistent sequence root;
- `source_record_index` is the immutable physical storage ordinal.

A logical move changes only sequence order. The moved record retains its physical source locator, payload identity, checkpoint identity, and physical height slot. Snapshots taken before a move remain immutable through the sequence copy-on-write root.

Insert/erase are still not exposed at the compact-arena boundary. This contract certifies durable **move/reorder**, not arbitrary structural editing.

## Durable logical-order generations

Logical order is persisted under `arena/` as generation files named:

`logical-order.g<16-hex-generation>.zmd`

A committed snapshot contains:

- magic `ZVORD001`;
- format version and exact header size;
- exact logical record count;
- a non-zero generation number;
- an `N × uint64` permutation of physical `source_record_index` values;
- a file-wide CRC32.

The parser rejects wrong magic/version/header size, wrong record count, malformed payload size, duplicate or out-of-range source indices, trailing/truncated bytes, generation mismatches, and CRC corruption.

If no committed generation exists, opening an older arena remains backward-compatible and reconstructs identity physical order as generation `0`.

## Publication and recovery

A move is published in this order:

1. fork the current immutable sequence root in O(1) with shared copy-on-write structure;
2. apply the move only to the candidate root;
3. serialize the candidate physical-source permutation for generation `current + 1`;
4. write a unique `.tmp` candidate and durably flush it;
5. publish a unique committed generation without overwriting an older committed generation;
6. reload and validate the committed generation exactly;
7. only then replace the live sequence root with the candidate.

Linux publication uses file `fsync`, no-replace hard-link publication, and directory `fsync`. Windows publication uses `FlushFileBuffers` followed by `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)` to the unique final generation path.

Recovery ignores torn higher `.tmp` candidates. It scans committed generation files, validates them fail-closed, and selects the highest committed generation. A corrupt committed generation is not silently ignored in favor of an older order.

If a durable publication call fails after entering the commit path, the reader marks itself closed rather than continuing from a potentially ambiguous publication outcome.

## Height persistence authority

Height correction is addressed by logical ordinal because layout and scroll code operate in logical order. Before touching persistent height state, `update_height()` resolves the logical record through the current sequence root and obtains its immutable `source_record_index`.

Persistent height state remains physically keyed:

- `record-heights.idx` offset = `source_record_index * entry_size`;
- `height-blocks.idx` block = `source_record_index / records_per_block`;
- block Fenwick bookkeeping remains physical-source keyed;
- the global total height remains valid because summation is independent of logical order;
- the sequence COW height update still targets the caller's current logical ordinal.

This prevents a moved logical record from overwriting another physical record's persisted height.

## Consumer authority

`LayoutWindowEngine` and `ZenithHotScrollSession` forward logical moves through the same compact arena authority.

Their source-derived paths remain physical:

- ordinary layout payload reads use `source_record_index`;
- ordinary layout cache identity includes logical and physical identity;
- checkpoint path/open identity uses `source_record_index`;
- hot-scroll checkpoint cache identity is physical-source keyed;
- hot-scroll raw source-window cache identity is physical-source keyed;
- emitted fragments retain the current logical `record_index`.

Therefore a moved record can appear at a new logical ordinal while continuing to read its original physical payload, checkpoint, and persisted height.

## Behavioral oracles

The admitted test chain proves:

1. sequence source identity survives move, erase, chunk split, snapshots, and COW root forks;
2. CompactArena materialization obtains order and height prefixes from the sequence authority;
3. moved records retain immutable physical payload identity through ordinary LayoutWindow reads and caches;
4. moved records retain physical checkpoint and source-window identity through hot-scroll;
5. height correction after a move writes the original physical source slot and block;
6. generation `1` and later moves survive reader reopen with the same logical order;
7. physical height updates survive reopen and recombine with the recovered logical permutation;
8. a same-index move creates no new generation;
9. a torn higher temporary generation is ignored;
10. a corrupt committed higher generation fails arena open closed;
11. newly opened LayoutWindow and HotScroll consumers recover the same durable logical order and continue reading the correct physical payload/checkpoint identities.

## Remaining M2 boundary

Durable move/reorder is no longer session-only. Remaining M2 admission work is repository-level closure rather than a missing move durability primitive:

1. complete the residual audit for logical-ordinal physical-source dereferences;
2. keep insert/erase outside the compact-arena contract unless they receive their own durable storage protocol;
3. verify the final branch diff against fresh `main` and exact-head CI;
4. produce final M2 evidence/promotion receipts only after those closure gates are green.
