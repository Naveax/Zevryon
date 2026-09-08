# M1/M2 logical-node source contract

## Purpose

The native ZMDOC header carries `logical_nodes`, `style_runs`, and related envelope counts, but it does not carry the actual browser-node semantics required by M1/M2. Those counts therefore remain validation metadata only. They must never be expanded into invented tags, attributes, roles, styles, topology, or source ranges.

`ZVNSRC01` is the explicit streaming handoff between a real parser/import producer and the disk-backed `ZVNODA01` logical-node arena. It exists so the parser side can publish actual semantic node events without requiring the arena builder to retain a browser object graph in RAM.

This slice deliberately does **not** close issue #145. A real parser/import producer and a production bounded consumer still have to use this contract before the two remaining M1 checkboxes are earned.

## Source identity

A `ZVNSRC01` stream is bound to the exact native-store logical payload SHA-256, not merely a file name, path, corpus count, or candidate label.

Import performs all of the following before arena publication:

1. opens the authoritative `StoreReader`;
2. decodes its exact `payload_sha256` receipt;
3. requires byte-for-byte equality with the node-source header digest;
4. requires the node-source node count to agree with store `logical_nodes` metadata;
5. validates every node's `source_record_index` against the physical record count;
6. validates `source_byte_offset + source_byte_length` against the referenced record through the bounded store slice authority;
7. feeds only the explicit node-source semantics into `LogicalNodeArenaWriter`.

The `logical_nodes` field is therefore a consistency check only. It is never a semantic producer.

## Binary format v1

All integers are little-endian.

### 72-byte header

- 8 bytes: magic `ZVNSRC01`
- `u32`: format version (`1`)
- `u32`: header bytes (`72`)
- `u64`: node count
- `u64`: attribute count
- 32 bytes: exact source payload SHA-256
- `u32`: reserved, zero
- `u32`: CRC32 over the first 68 bytes

### Node frame

Each node is one bounded CRC-protected frame. Frames are emitted in contiguous 1-based pre-order logical-ID order.

Fixed prefix:

- `u32`: total frame bytes, including trailing CRC
- `u32`: attribute count
- `u64`: logical ID
- `u64`: physical source record index
- `u64`: source byte offset within that record
- `u64`: source byte length
- `u64`: parent ordinal, or `kNoLogicalNodeOrdinal` for the root
- `u32`: node flags
- `u32`: tag byte length
- `u32`: role byte length
- `u32`: style byte length

Variable payload:

1. tag UTF-8 bytes;
2. role UTF-8 bytes;
3. style UTF-8 bytes;
4. zero or more attribute records.

Each attribute record contains:

- `u32`: name byte length;
- `u32`: value byte length;
- `u32`: flags;
- `u32`: reserved, zero;
- name bytes;
- value bytes.

The frame ends with CRC32 over every preceding byte in that frame.

## Boundedness and fail-closed rules

- Semantic strings use the same 1 MiB per-value ceiling as the arena interner.
- A complete node-source frame is capped at 16 MiB.
- Empty tags and empty attribute names are rejected.
- Source byte-range integer overflow is rejected.
- IDs must be contiguous 1-based pre-order IDs.
- Root/parent ordering is checked by the source contract; the arena independently enforces the stronger open-ancestor topology invariant.
- Header CRC, frame CRC, reserved fields, manifest counts and trailing bytes are validated.
- The final arena remains create-only and is not published after any source identity, topology, range, CRC or semantic failure.

## Production integration boundary still open

The repository currently has no HTML/DOM parser implementation that emits real tag/attribute/role/style nodes. The existing `native-dom` benchmark path is record/checkpoint based and is not evidence of a semantic DOM producer.

The next admission slice must connect an actual parser/import producer to `LogicalNodeSourceWriter` (or an equivalent source adapter preserving this contract) and then route a bounded production consumer through `LogicalNodeArenaReader`. Until both ends exist, issue #145 and the two M1 semantic-arena checkboxes remain open.
