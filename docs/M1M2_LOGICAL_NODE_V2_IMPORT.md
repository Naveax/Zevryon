# M1/M2 ZVNSRC01 v2 to store-bound ZVNODA v2 import

## Contract

`import_logical_node_source_v2_to_arena_v2()` converts an explicit `ZVNSRC01` v2 stream into the authoritative physical-store-bound `node-arena-v2/` representation.

The importer never synthesizes nodes from envelope counts. It replays exact source nodes, topology, flags, tag/role/style semantics and complete attribute slices from the versioned source stream.

## Single-pass source authority

The importer keeps one `LogicalNodeSourceV2Reader` open from the first untrusted frame through the final node. It does not validate `source_path` and later reopen the path for a second replay pass.

Before node replay it requires:

- source payload SHA-256, physical record-sequence SHA-256 and record count to match the authoritative native store;
- source node count to match native-store `logical_nodes` metadata;
- the store-bound arena writer's independently re-inspected binding to remain exactly equal to the source binding.

For each decoded node, the importer validates that exact node's source span through bounded `StoreReader::read_record_span()` delivery before copying the node or any of its semantics into arena staging. Frame CRC, topology and semantic bounds remain enforced by `LogicalNodeSourceV2Reader` on the same open stream.

This removes the previous validate-by-path/reopen-by-path TOCTOU boundary: the bytes decoded and validated are the bytes immediately replayed into the arena.

## Failure behavior

Invalid candidate identity, source/store binding mismatch or source/store count mismatch fails before arena staging begins.

A malformed or escaping later node may be discovered after earlier valid nodes have entered `node-arena-v2.building/`. That staging tree is never authoritative: any failure before `finish()` destroys the incomplete writer state, removes staging, and cannot publish `node-arena-v2/`.

The invariant is therefore stronger and simpler: no node is appended before its own source span validates, and no partially validated source can publish an authoritative arena.

## Boundedness

Source range validation is streaming and bounded by StoreReader's I/O window. The importer materializes one decoded logical source node and its attribute slice at a time; it never retains the complete browser-node graph in memory.

Semantic interning remains the disk-backed/spillable arena implementation's responsibility. `semantic_hash_bits = 0` remains supported for forced-collision correctness tests; values above 64 are rejected.

## Tests

Focused authority covers:

- cross-record text span validation and import in the same replay pass;
- topology, node flags, attributes, tag, role and style round trip;
- exact payload SHA, physical record-sequence SHA and record-count binding after arena reopen;
- a late escaping source span after an earlier valid node, proving no final arena and no staging tree remain after failure;
- source/store binding mismatch rejected before staging;
- invalid configuration rejected before staging;
- forced semantic-hash collision configuration remains exact.

## Status

This slice depends on the admitted explicit `ZVNSRC01` v2 stream and the store-bound v2 arena. Normal HTML data-state text production is a separate Z7 slice. Raw-text/RCDATA/WHATWG conformance remains later work.
