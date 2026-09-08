# M1/M2 disk-backed logical-node arena

## Scope

This document freezes the first infrastructure slice for issue #145. It adds a real disk-backed logical-node metadata sidecar and deterministic semantic interning without changing the existing `ZMDOC001` payload format or claiming that browser/parser integration is complete.

The sidecar is optional until a production parser/node-source emits real node semantics. The `logical_nodes` count in an existing MassiveDoc manifest is an envelope/count field and is **not** sufficient evidence from which to invent production nodes.

## Format identity

The logical-node sidecar lives under `node-arena/` and uses manifest magic:

`ZVNODA01`

Format version is `1`.

Publication is create-only:

1. a writer builds `node-arena.building/`;
2. every node, attribute, semantic dictionary and manifest is completed there;
3. only a successful `finish()` renames the complete directory to `node-arena/`;
4. an existing authoritative `node-arena/` is never overwritten;
5. an unfinished/failed writer may not publish authoritative output.

`ZMDOC001` compatibility is unchanged by this slice.

## Node identity and topology

Version 1 uses a streaming pre-order contract:

- node ordinal is zero-based on disk;
- public logical node id is fixed to `ordinal + 1`;
- logical id `0` is invalid;
- the first node is the single root and has no parent;
- every later node names a parent whose ordinal already exists and is still an open pre-order ancestor;
- topology is stored as parent, first-child and next-sibling ordinals;
- `UINT64_MAX` is the absent topology sentinel.

The 1-based identity rule deliberately makes `logical_id -> ordinal` arithmetic and avoids a RAM-resident per-node identity index.

Each fixed-width node record stores:

- logical id;
- source record index;
- source byte offset and length;
- parent ordinal;
- first-child ordinal;
- next-sibling ordinal;
- attribute slice offset/count;
- interned tag, role and style ids;
- flags;
- reserved bytes;
- CRC32.

The reader rejects invalid ids, impossible forward/backward topology, source-range overflow, attribute-slice escape, semantic-id escape, reserved-byte drift and CRC mismatch.

## Attribute arena

Attributes are written to a separate fixed-width record array. Each record stores:

- interned attribute-name id;
- interned attribute-value id;
- flags;
- CRC32.

Attribute names must be non-empty. Attribute values are always interned, including the empty string, so an explicit empty value remains distinct from an absent optional node semantic.

## Semantic interning

Five independent deterministic dictionaries are used:

1. tag;
2. role;
3. style;
4. attribute name;
5. attribute value.

ID `0` is reserved for absent optional node semantics. Real dictionary entries start at ID `1`.

The writer keeps only a fixed-size bucket-head table in resident memory. Dictionary entries, collision chains and the ID-to-entry offset index are disk-backed. Increasing the number of nodes or unique semantic values does not grow the RAM bucket-head array.

A hash may select a bucket and accelerate lookup, but hash equality never defines semantic identity. Every candidate collision is accepted as the same interned value only after exact byte comparison.

The test authority can deliberately reduce the semantic hash to zero bits, forcing every value through one collision chain while still requiring distinct values to receive distinct ids and repeated exact values to reuse ids.

## Dictionary integrity

Each dictionary entry binds:

- fixed-width id;
- hash value;
- next collision-chain offset;
- bounded payload length;
- exact payload bytes;
- CRC32 over header identity fields plus payload.

A separate fixed-width offset file maps `id -> entry offset`. The reader validates exact offset-file size, range, entry id, payload bounds and CRC before returning a semantic value.

Hash collisions, truncated offset indexes, escaped offsets, corrupt lengths and payload tampering fail closed.

## Candidate/source identity

`manifest.bin` binds the arena to:

- exact candidate commit;
- exact candidate tree;
- 32-byte source SHA-256 identity;
- node and attribute counts;
- exact per-dictionary semantic counts;
- semantic bucket/hash configuration;
- fixed record sizes;
- manifest CRC32.

Candidate commit/tree fields are required to be exact 40-hex Git identities.

This first slice records and exposes those identities. A later production node-source integration must define which source artifact supplies the SHA-256 and must verify it against the parser/import authority rather than hand-authoring it.

## Bounded-memory boundary

The writer keeps:

- one fixed bucket-head array per dictionary;
- a pre-order ancestor stack proportional to current tree depth;
- one node/attribute/dictionary entry under construction.

It does not retain a heap object for every logical node and does not retain every semantic dictionary entry in memory.

The reader performs bounded random-access reads for one node, one attribute or one semantic value. It does not load the complete node or dictionary table on open.

## CI authority

`logical-node-arena-tests` is a non-certifying implementation test. It exercises the actual production writer/reader path with:

- repeated semantic id reuse;
- forced all-value hash collision behavior;
- parent/child/sibling topology round-trip after reopen;
- explicit empty attribute-value identity;
- create-only publication;
- multiple-root rejection;
- manifest CRC tampering;
- dictionary payload CRC tampering;
- dictionary offset-index truncation;
- attribute range failure;
- a root plus 20,000 shallow child nodes while proving the semantic bucket-head memory size remains fixed.

Hosted CI does **not** certify the final 67,108,864-node envelope or physical memory behavior.

## Remaining issue #145 boundary

This infrastructure slice does not close the two M1/M2 plan items. They remain open until:

- a real production parser/import node-source streams actual browser node semantics into this sidecar;
- at least one production layout/accessibility/browser consumer reads semantic/topology metadata through the bounded arena instead of resident per-node semantic objects;
- repeated tag/attribute/role/style data is demonstrably interned in that production path;
- the production integration preserves source-byte fidelity, logical order, selection, copy, search and export behavior.

Only then may the execution-plan checkboxes be closed. The final 67,108,864-node certification envelope remains a separate real evidence gate.
