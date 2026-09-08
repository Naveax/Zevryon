# M1/M2 ZVNSRC01 v2 to store-bound ZVNODA v2 import

## Contract

`import_logical_node_source_v2_to_arena_v2()` converts an explicit `ZVNSRC01` v2 stream into the authoritative physical-store-bound `node-arena-v2/` representation.

The importer never synthesizes nodes from envelope counts. It replays exact source nodes, topology, flags, tag/role/style semantics and complete attribute slices from the versioned source stream.

## Two independent authority passes

Before any arena staging directory is created, the importer runs `validate_logical_node_source_v2_against_store()` to completion. This verifies the v2 source binding and every cross-record source span against the authoritative native store using bounded `StoreReader::read_record_span()` delivery.

Only after that pass succeeds does the importer open `LogicalNodeArenaV2StoreBoundWriter`.

The destination writer independently re-inspects the native store and freezes payload SHA-256, physical record-sequence SHA-256 and physical record count into the arena's store-bound identity. Therefore validation of the input source and binding of the output arena are separate checks rather than one trusted handoff.

## Failure behavior

Malformed candidate identity, invalid semantic interning configuration, source/store binding mismatch, escaping cross-record span or corrupt source data fails before `node-arena-v2.building/` exists.

Failures after staging begins are cleaned by the create-only arena writer and cannot publish a partial authoritative arena.

## Boundedness

Source range validation is streaming and bounded by StoreReader's I/O window. The importer materializes one decoded logical source node and its attribute slice at a time; it never retains the complete browser-node graph in memory.

Semantic interning remains the disk-backed/spillable arena implementation's responsibility. `semantic_hash_bits = 0` remains supported for forced-collision correctness tests; values above 64 are rejected.

## Tests

Focused authority covers:

- cross-record text span source validation and import;
- topology, node flags, attributes, tag, role and style round trip;
- exact payload SHA, physical record-sequence SHA and record-count binding after arena reopen;
- escaping source span rejected before staging;
- source/store binding mismatch rejected before staging;
- invalid configuration rejected before staging;
- forced semantic-hash collision configuration remains exact.

## Status

This slice depends on the admitted explicit `ZVNSRC01` v2 stream and the store-bound v2 arena. Normal HTML data-state text production is a separate Z7 slice. Raw-text/RCDATA/WHATWG conformance remains later work.
