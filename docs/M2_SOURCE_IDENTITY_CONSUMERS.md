# M2 — Physical Source Identity Consumer Split

This pass begins the consumer-side separation between mutable logical document order and immutable MassiveDoc payload identity.

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

## Oracle

The checkpoint test intentionally sets:

- logical ordinal = `9001`;
- physical source record index = `0`.

The only fixture payload exists at physical record `0`. The scan must succeed, read that source record, and return fragments whose logical `record_index` remains `9001`. The same test then changes only the physical source identity to `1` and requires a fail-closed identity mismatch.

This makes the distinction behavioral rather than documentary.

## Remaining consumer split

This pass does not admit arena reorder yet. Remaining consumers must be migrated and certified independently:

1. ordinary `LayoutWindowEngine` payload reads and cache identity;
2. Zenith checkpoint path/open selection;
3. hot-scroll source-window reads and cache identity;
4. any other source-derived cache/checkpoint keys discovered by the audit.

Only after those consumers use immutable source identity may logical move/reorder be exposed at the compact-arena boundary.
