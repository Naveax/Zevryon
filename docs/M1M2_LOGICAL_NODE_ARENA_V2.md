# ZVNODA logical-node arena v2 wrapper

## Status

This is a compatibility-preserving arena extension for cross-record source-span semantics. It does not mutate the admitted `ZVNODA01` v1 binary storage implementation.

## Isolation model

The v2 authoritative path is:

`node-arena-v2/`

Inside it, the admitted v1 disk-backed storage engine is reused at:

`node-arena-v2/node-arena/`

The outer directory also contains:

`node-arena-v2/manifest-v2.bin`

An old `LogicalNodeArenaReader(store_root)` only looks for `store_root/node-arena/`, so it cannot silently open v2 data with v1 source-range semantics.

## V2 marker

`manifest-v2.bin` uses magic `ZVNODV02`, format version 2 and CRC32 protection. It binds the nested storage manifest fields:

- nested storage format version;
- explicit cross-record source-span semantics id;
- semantic bucket count and hash bits;
- node and attribute counts;
- all five semantic dictionary counts;
- candidate commit and tree;
- source SHA-256.

The v2 reader opens the nested v1 storage only after decoding the marker, then requires exact equality between every bound marker field and the actual nested storage manifest. A valid-CRC marker transplanted from a different arena therefore fails closed.

## Source-span semantics

The fixed node record remains unchanged. V2 interprets:

`(source_record_index, source_byte_offset, source_byte_length)`

as a contiguous logical byte span whose length may continue through following physical native-store records.

The arena itself stores this triple without trying to reinterpret physical store boundaries. Validation against the authoritative native store belongs to the versioned source/import path.

## Publication

`LogicalNodeArenaV2Writer` publishes atomically:

1. clear stale `node-arena-v2.building/`;
2. build nested v1 storage under `node-arena-v2.building/node-arena/`;
3. close the nested reader used to inspect its completed manifest;
4. write `manifest-v2.bin`;
5. recheck that `node-arena-v2/` did not appear concurrently;
6. rename the complete outer staging directory to `node-arena-v2/`.

The nested reader is deliberately destroyed before the outer directory rename. This is required for Windows, where an open file handle can prevent directory publication.

An existing authoritative v2 arena is never overwritten.

## Reader

`LogicalNodeArenaV2Reader`:

1. reads and validates the v2 marker;
2. opens the nested v1 storage engine;
3. compares the complete marker binding against the nested storage manifest;
4. delegates bounded node, attribute and semantic reads to the existing disk-backed reader.

This keeps the proven fixed-width node/attribute and semantic-dictionary implementation while making the source-span semantic version explicit.

## Tests

The focused v2 test authority covers:

- atomic v2 publication with no accidental v1 root publication;
- rejection by the old v1 reader;
- v2 reopen and node/semantic round trip;
- preservation of a cross-record source-span triple;
- create-only publication;
- marker corruption rejection;
- valid-CRC marker transplant rejection through nested-manifest binding.

## Admission boundary

This wrapper alone does not import `ZVNSRC01` v2 and does not make the HTML producer emit text nodes.

The next coherent integration is:

1. admit the explicit ZVNSRC01 v2 source stream;
2. add v2 source-to-arena import with bounded StoreReader span validation;
3. switch normal HTML data-state text nodes to the v2 producer path;
4. continue raw-text/RCDATA and WHATWG parser work;
5. connect a bounded production consumer of the arena.

Z7 remains planned. GitHub issue #145 is already closed/completed on canonical history; this wrapper is follow-on versioned arena work and does not claim the later 67,108,864-node certification envelope.
