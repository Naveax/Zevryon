# ZVNODA logical-node arena v2 wrapper

## Status

This is a compatibility-preserving arena extension for cross-record source-span semantics. It does not mutate the admitted `ZVNODA01` v1 binary storage implementation.

## Isolation model

The v2 authoritative path is `node-arena-v2/`. The admitted v1 disk-backed storage engine is nested at `node-arena-v2/node-arena/`. The outer directory also contains `manifest-v2.bin` and the authoritative `source-binding-v2.bin`.

An old `LogicalNodeArenaReader(store_root)` only looks for `store_root/node-arena/`, so it cannot silently open v2 data with v1 source-range semantics.

## Low-level marker

`manifest-v2.bin` uses magic `ZVNODV02`, format version 2 and CRC32 protection. It binds the nested storage manifest fields including storage format, cross-record semantics id, semantic bucket/hash configuration, node/attribute/dictionary counts, candidate commit/tree and the nested storage source hash.

The low-level arena implementation is no longer directly constructible by production callers. `LogicalNodeArenaV2Writer` and `LogicalNodeArenaV2Reader` are storage primitives accessible only to the store-bound wrapper.

## Exact native-store binding

Cross-record source triples depend on physical record boundaries. Payload SHA-256 alone is therefore insufficient.

`LogicalNodeArenaV2StoreBoundWriter` derives the admitted `LogicalNodeSourceStoreBinding` directly from the authoritative native store:

- exact payload SHA-256;
- exact physical record-sequence SHA-256;
- exact physical record count.

It computes a domain-separated composite identity:

`SHA256("ZEVRYON-ZVNODA02-SOURCE-BINDING" || payload_sha256 || record_sequence_sha256 || record_count_le64)`

The nested arena's existing 32-byte source identity field stores this composite hash. `source-binding-v2.bin` stores the three source-binding components with a fixed schema and CRC32. The sidecar is written inside `node-arena-v2.building/`, so it is published atomically with the completed outer arena.

`LogicalNodeArenaV2StoreBoundReader::open()` requires all of the following before exposing nodes:

1. the fixed source-binding sidecar decodes and passes CRC;
2. its payload SHA, physical record-sequence SHA and record count exactly match the current native store;
3. the domain-separated composite hash recomputed from that binding exactly matches the nested arena source identity;
4. the existing v2 marker still exactly binds the nested storage manifest.

This prevents both whole-arena transplant across different physical partitions and a Frankenstein combination where an arena from partition A is paired with a valid-CRC source-binding sidecar from partition B.

## Source-span semantics

The fixed node record remains unchanged. V2 interprets `(source_record_index, source_byte_offset, source_byte_length)` as a contiguous logical byte span whose length may continue through following physical native-store records.

## Publication

The authoritative store-bound writer:

1. inspects the native store and verifies the caller's payload SHA against it;
2. computes the composite source identity;
3. starts low-level construction under `node-arena-v2.building/`;
4. writes `source-binding-v2.bin` into that same staging tree;
5. completes nested storage and `manifest-v2.bin`;
6. closes staging readers before publication;
7. atomically renames the complete outer staging directory to `node-arena-v2/`.

Existing authoritative v2 output is never overwritten.

## Tests

Focused authority covers:

- atomic store-bound publication with no accidental v1 root;
- old v1 reader isolation;
- source triple and semantic round trip;
- exact record-count and record-sequence binding after reopen;
- create-only publication;
- source-binding CRC tamper rejection;
- v2 marker CRC tamper rejection;
- two stores with identical payload bytes but different physical partitions;
- valid-CRC sidecar/arena mixing across those partitions rejected by the composite identity.

## Admission boundary

This wrapper does not yet import `ZVNSRC01` v2 or make the HTML producer emit text nodes. The next coherent integration is the validated v2 source-to-arena importer followed by normal data-state text-node production and later raw-text/RCDATA/WHATWG work.

Z7 remains planned. GitHub issue #145 is already closed/completed on canonical history; this is follow-on versioned arena hardening and does not claim the later 67,108,864-node certification envelope.
