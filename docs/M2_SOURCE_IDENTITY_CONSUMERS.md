# M2 — Physical Source Identity Consumer Split

This line separates mutable logical document order from immutable MassiveDoc payload identity consumer by consumer. Each consumer is admitted independently so a source-identity mistake cannot hide behind a broad reorder change.

## Identity rule

`MaterializedRecord::record_index` is the current logical ordinal used for ordering, anchors, fragments and height correction.

`MaterializedRecord::source_record_index` is the immutable physical `records.idx` ordinal used to dereference source payload and source-derived persistent artifacts.

A consumer must not substitute one identity for the other after logical reordering becomes available.

## Checkpoint scan admission

`scan_layout_window_from_checkpoint()` now matches checkpoint physical identity against `source_record_index`, reads MassiveDoc slices with that physical locator, and keeps `LayoutFragment::record_index` logical. The v1 checkpoint header field remains named `record_index` for format compatibility but semantically stores the immutable physical source record.

The checkpoint oracle intentionally uses logical ordinal `9001` with physical source record `0`. The only payload exists at physical record `0`; scan succeeds and still emits logical fragment index `9001`. Changing only the physical source identity to `1` fails closed.

## Ordinary LayoutWindow admission

`LayoutWindowEngine` treats the identities separately:

- `StoreReader::read_record()` dereferences `source_record_index`;
- generated fragments, scroll anchors and arena height correction retain logical `record_index`;
- the layout cache key contains both logical and physical identity plus layout configuration.

Both identities are required because cached fragments embed logical order while their geometry derives from physical payload. A moved record therefore cannot reuse fragments stamped with its old logical ordinal.

This pass is preparatory because public arena reorder is still closed; the final moved-record integration oracle remains mandatory before reorder admission.

## Zenith checkpoint-aware layout admission

`layout_window_with_persistent_checkpoints()` now selects source-derived checkpoint state by immutable physical identity:

- checkpoint path lookup uses `source_record_index`;
- checkpoint open verification uses `source_record_index`;
- checkpoint index-byte charging/deduplication is keyed by `source_record_index`;
- scroll anchors, layout fragments and height correction continue to use logical `record_index`.

This matches checkpoint scan semantics and prevents a logical move from looking for a checkpoint under the moved ordinal rather than the payload record that generated it.

## Remaining consumer split

Arena reorder remains closed. Remaining work:

1. hot-scroll source-window reads and raw-source cache identity;
2. audit any remaining source-derived cache/checkpoint keys;
3. add a final end-to-end moved-record oracle across ordinary layout, checkpoint-aware layout and hot-scroll;
4. only then expose logical move/reorder at the compact-arena boundary.

Durability/journaling for logical-order mutations remains a later M2 storage-hardening pass.
