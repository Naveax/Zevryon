# M2 — Physical Source Identity Consumer Split

This line separates mutable logical document order from immutable MassiveDoc payload identity consumer by consumer. Each consumer is admitted independently so a source-identity mistake cannot hide behind a broad reorder change.

## Identity rule

`MaterializedRecord::record_index` is the current logical ordinal used for ordering, anchors, fragments and height correction.

`MaterializedRecord::source_record_index` is the immutable physical `records.idx` ordinal used to dereference source payload and source-derived persistent artifacts.

A consumer must not substitute one identity for the other after logical reordering becomes available.

## Checkpoint scan admission

`scan_layout_window_from_checkpoint()` now:

- matches the checkpoint header's physical record identity against `source_record_index`;
- reads MassiveDoc slices with `source_record_index`;
- continues to stamp produced `LayoutFragment::record_index` with the current logical ordinal;
- rejects a checkpoint whose physical source identity differs even when logical metadata otherwise matches.

The checkpoint on-disk header field remains named `record_index` in the v1 format for compatibility, but in this boundary it represents the immutable physical source-record index. A later format cleanup may rename the field without changing this semantic authority.

The checkpoint test intentionally sets logical ordinal `9001` and physical source record index `0`. The only fixture payload exists at physical record `0`; the scan succeeds, reads that source record and still emits fragments with logical `record_index=9001`. Changing only the physical source identity to `1` fails closed.

## Ordinary LayoutWindow admission

`LayoutWindowEngine` now treats the two identities separately:

- `StoreReader::read_record()` dereferences `source_record_index`;
- generated fragments continue to carry logical `record_index`;
- scroll-anchor comparisons and arena height correction continue to use logical `record_index`;
- the layout cache key contains **both** logical and physical identity plus the layout bucket/configuration.

Both identities are required in the cache key because cached fragments embed the logical ordinal while their text geometry derives from the immutable physical payload. A moved record must therefore miss an entry created under its old logical ordinal rather than replaying fragments stamped with stale order identity. Conversely, two logical records must not alias merely because they point at the same physical source identity.

This pass is preparatory: current compact-arena order is still initially identical to physical source order, so the existing layout corpus cannot create a divergent logical/physical pair through the public arena API yet. The earlier checkpoint oracle supplies the behavioral divergence proof for the source identity model; final end-to-end LayoutWindow divergence will be required when arena move/reorder is admitted.

## Remaining consumer split

Arena reorder is still closed. Remaining consumers must be migrated and certified independently:

1. Zenith checkpoint path/open selection and physical index accounting;
2. hot-scroll source-window reads and cache identity;
3. any other source-derived cache/checkpoint keys discovered by the audit;
4. final end-to-end moved-record oracle across ordinary layout, checkpoint-aware layout and hot-scroll.

Only after those consumers use immutable source identity may logical move/reorder be exposed at the compact-arena boundary.
