# M1/M2 logical-node source contract

## Purpose

The native ZMDOC header carries `logical_nodes`, `style_runs`, and related envelope counts, but it does not carry the actual browser-node semantics required by M1/M2. Those counts remain validation metadata only. They must never be expanded into invented tags, attributes, roles, styles, topology, or source ranges.

`ZVNSRC01` is the explicit streaming handoff between a real parser/import producer and the disk-backed `ZVNODA01` logical-node arena. It lets the parser side publish actual semantic node events without requiring the arena builder to retain a browser object graph in RAM.

This slice deliberately does **not** close issue #145. A real parser/import producer and a production bounded consumer still have to use this contract before the remaining M1 semantic-arena checkboxes are earned.

## Source identity

A `ZVNSRC01` stream has two independent immutable store bindings.

### Logical payload identity

`payload_sha256` binds the exact concatenation of native-store record payload bytes. It prevents the semantic sidecar from being attached to a different document payload.

### Physical record-sequence identity

A payload SHA alone cannot identify record boundaries. For example, records `['ab', 'c']` and `['a', 'bc']` have the same concatenated payload but different meanings for `source_record_index` and byte offsets.

`record_sequence_sha256` therefore hashes a domain-separated canonical stream containing:

- physical record count;
- each physical record ordinal;
- each record's stored logical ID;
- each record's byte length;
- each record's payload CRC32.

The digest deliberately excludes `first_chunk`, `chunk_count`, segment IDs and segment offsets. Those are storage-placement details that may change during compaction while the stable source-record sequence remains identical.

Import requires both digests and the source-record count to match the authoritative store before arena publication. `logical_nodes` remains only a count consistency check and is never a semantic producer.

## Import validation sequence

Before `node-arena/` can be published, import:

1. opens the authoritative native store;
2. obtains its exact payload SHA-256;
3. computes the bounded physical record-sequence SHA-256 from `records.idx`;
4. requires exact equality with all `ZVNSRC01` store-binding fields;
5. requires node-source node count to agree with store `logical_nodes` metadata;
6. requires every node's `source_record_index` to fit the bound physical record count;
7. validates `source_byte_offset + source_byte_length` against the referenced record through the bounded StoreReader slice authority;
8. feeds only explicit node-source semantics into `LogicalNodeArenaWriter`.

## Binary format v1

All integers are little-endian.

### 112-byte header

- 8 bytes: magic `ZVNSRC01`;
- `u32`: format version (`1`);
- `u32`: header bytes (`112`);
- `u64`: node count;
- `u64`: attribute count;
- `u64`: physical source-record count;
- 32 bytes: exact logical payload SHA-256;
- 32 bytes: stable physical record-sequence SHA-256;
- `u32`: reserved, zero;
- `u32`: CRC32 over the first 108 bytes.

This incompatible header expansion is still format version 1 because `ZVNSRC01` has not yet been admitted to `main`; there is no released v1 authority to preserve.

### Node frame

Each node is one bounded CRC-protected frame. Frames are emitted in contiguous 1-based pre-order logical-ID order.

Fixed prefix:

- `u32`: total frame bytes, including trailing CRC;
- `u32`: attribute count;
- `u64`: logical ID;
- `u64`: physical source record index;
- `u64`: source byte offset within that record;
- `u64`: source byte length;
- `u64`: parent ordinal, or `kNoLogicalNodeOrdinal` for the root;
- `u32`: node flags;
- `u32`: tag byte length;
- `u32`: role byte length;
- `u32`: style byte length.

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
- One node is capped at 65,536 attributes.
- Reader attribute allocation is allowed only after the declared count is proven to fit both the remaining manifest count and the current frame's minimum encoded bytes.
- Empty tags and empty attribute names are rejected.
- Source byte-range integer overflow is rejected.
- IDs must be contiguous 1-based pre-order IDs.
- Root/parent ordering is checked by the source contract; the arena independently enforces the stronger open-ancestor topology invariant.
- Header CRC, frame CRC, reserved fields, manifest counts and trailing bytes are validated.
- An unfinished writer removes its `.building` file and cannot publish a partial source stream.
- The final arena remains create-only and is not published after any source identity, topology, range, CRC or semantic failure.

## Test authority in this slice

The focused tests cover:

- source-to-arena semantic round trip and interning reuse;
- payload SHA mismatch rejection;
- equal concatenated payload with different physical record partition rejection;
- record range escape rejection;
- frame CRC corruption rejection;
- forged oversized attribute count with a recomputed valid frame CRC;
- unfinished writer cleanup.

## Production integration boundary still open

The repository currently has no HTML/DOM parser implementation that emits real tag/attribute/role/style nodes. The existing `native-dom` benchmark path is record/checkpoint based and is not evidence of a semantic DOM producer.

The next admission slice must connect an actual parser/import producer to `LogicalNodeSourceWriter` (or an equivalent source adapter preserving this contract) and then route a bounded production consumer through `LogicalNodeArenaReader`. Until both ends exist, issue #145 and the remaining M1 semantic-arena work stay open.
