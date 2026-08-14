# M2 — Physical Source Identity Consumer Split

This line separates mutable logical document order from immutable MassiveDoc payload identity consumer by consumer. Each consumer is admitted independently so a source-identity mistake cannot hide behind a broad reorder change.

## Identity rule

`MaterializedRecord::record_index` is the current logical ordinal used for ordering, anchors, fragments and height correction.

`MaterializedRecord::source_record_index` is the immutable physical `records.idx` ordinal used to dereference source payload and source-derived persistent artifacts.

A consumer must not substitute one identity for the other after logical reordering becomes available.

## Checkpoint scan admission

`scan_layout_window_from_checkpoint()` matches checkpoint physical identity against `source_record_index`, reads MassiveDoc slices with that physical locator, and keeps `LayoutFragment::record_index` logical. The v1 checkpoint header field remains named `record_index` for format compatibility but semantically stores the immutable physical source record.

The checkpoint oracle intentionally uses logical ordinal `9001` with physical source record `0`. The only payload exists at physical record `0`; scan succeeds and still emits logical fragment index `9001`. Changing only the physical source identity to `1` fails closed.

## Ordinary LayoutWindow admission

`LayoutWindowEngine` treats the identities separately:

- `StoreReader::read_record()` dereferences `source_record_index`;
- generated fragments, scroll anchors and arena height correction retain logical `record_index`;
- the layout cache key contains both logical and physical identity plus layout configuration.

Both identities are required because cached fragments embed logical order while their geometry derives from physical payload. A moved record therefore cannot reuse fragments stamped with its old logical ordinal.

## Zenith checkpoint-aware layout admission

`layout_window_with_persistent_checkpoints()` selects source-derived checkpoint state by immutable physical identity:

- checkpoint path lookup uses `source_record_index`;
- checkpoint open verification uses `source_record_index`;
- checkpoint index-byte charging/deduplication is keyed by `source_record_index`;
- scroll anchors, layout fragments and height correction continue to use logical `record_index`.

This prevents a logical move from looking for a checkpoint under the moved ordinal instead of the payload record that generated it.

## Zenith hot-scroll admission

`ZenithHotScrollSession` now separates source-derived cache identity from logical layout identity:

- checkpoint cache keys use physical `source_record_index` plus width bucket;
- checkpoint cache hits explicitly verify the cached checkpoint's physical record identity;
- checkpoint path/open use physical `source_record_index`;
- raw source-window cache keys use physical `source_record_index`, source offset and request length;
- `StoreReader::read_record_slice()` uses physical `source_record_index`;
- hot-scroll checkpoint validation compares the checkpoint's physical record to `source_record_index`;
- checkpoint index-byte deduplication uses physical identity in both layout passes.

The checkpoint and raw-window caches intentionally do **not** include logical ordinal: neither cache stores fragments stamped with logical order. Reordering a logical record should therefore continue to reuse the same immutable checkpoint/raw bytes. Fragment emission, scroll anchors and arena height correction remain logical and continue to use `record_index`.

Existing hot-scroll tests continue to certify checkpoint reuse, zero-I/O repeated queries, adjacent-scroll raw-window reuse, byte budgets and safe fallback. A final divergent logical/physical moved-record oracle remains required before public reorder is admitted because the current arena API still keeps initial logical and physical order equal.

## Remaining consumer split

Arena reorder remains closed. Remaining work:

1. audit the repository for any other source-derived cache/checkpoint/store reads still keyed by logical ordinal;
2. if the audit is empty, expose a narrowly scoped in-memory logical move/reorder boundary in `CompactArenaReader` while preserving immutable physical source identity;
3. add an end-to-end moved-record oracle across ordinary layout, checkpoint-aware layout and hot-scroll proving payload/checkpoint/raw-window identity stays physical while fragments/anchors stay logical;
4. only after the moved-record oracle passes may logical reorder receive runtime admission.

Crash-safe journaling and durable logical-order mutation persistence remain a later M2 storage-hardening pass and receive no credit from these in-memory consumer migrations.
