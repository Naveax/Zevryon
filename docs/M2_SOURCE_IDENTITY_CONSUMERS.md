# M2 — Physical Source Identity Consumer Authority

M2 separates mutable logical document order from immutable MassiveDoc storage identity all the way through layout, checkpoints, hot-scroll caches, height persistence, and durable reopen.

## Identity rule

`MaterializedRecord::record_index` is the current logical ordinal. It owns ordering, scroll anchors, emitted fragment ordinals, and logical sequence updates.

`MaterializedRecord::source_record_index` is the immutable physical `records.idx` ordinal. It owns payload dereference and source-derived persistent/cache identity.

Logical reorder must never reinterpret a logical ordinal as a physical source locator.

## Checkpoint scan

`scan_layout_window_from_checkpoint()` verifies checkpoint physical identity against `source_record_index`, reads MassiveDoc slices with that physical locator, and emits logical `LayoutFragment::record_index`.

The checkpoint format's v1 `record_index` header field is retained for compatibility but semantically identifies the physical source record.

The divergent checkpoint oracle uses logical ordinal `9001` with physical source record `0`, proving scan identity is physical while fragment identity stays logical.

## Ordinary LayoutWindow

`LayoutWindowEngine` keeps the identities separate:

- `StoreReader::read_record()` uses `source_record_index`;
- generated fragments and scroll anchors use logical `record_index`;
- height correction is invoked by logical ordinal and resolves physical storage inside the arena;
- cache identity includes both logical and physical record identity plus layout configuration because cached fragments embed logical ordinals while geometry derives from physical payload.

A moved-record oracle swaps two logical records whose payloads differ in newline behavior. The moved fragments expose their new logical ordinals while retaining the newline behavior of their original physical payloads.

## Zenith checkpoint-aware layout

Persistent checkpoint lookup and verification are source-derived:

- checkpoint path uses `source_record_index`;
- checkpoint open verification uses `source_record_index`;
- checkpoint scan reads source slices with `source_record_index`;
- checkpoint byte charging/deduplication uses physical source identity;
- anchors, fragments and logical height-update calls remain logical.

## Zenith hot-scroll

`ZenithHotScrollSession` uses physical identity for source-derived state:

- checkpoint cache key = physical `source_record_index` + width bucket;
- checkpoint hits verify the cached checkpoint's physical record;
- checkpoint path/open use physical source identity;
- raw source-window cache key = physical source + source offset + request length;
- `StoreReader::read_record_slice()` uses physical source identity;
- checkpoint scan validates physical source identity;
- checkpoint index-byte deduplication uses physical identity.

These caches intentionally omit logical ordinal because they do not store fragments stamped with logical order. A logical move can therefore reuse the same immutable checkpoint/raw bytes while fragment emission uses the new logical ordinal.

The moved hot-scroll oracle uses two distinct physical records and proves independent checkpoint/raw-window cache identity, correct moved logical fragments, and physical payload behavior.

## Runtime prefetch and semantic adoption

`ZenithTabRuntime` preserves the same identity rule outside the layout engine:

- source-window prefetch requests copy `LayoutFragment::source_record_index` into the physical request locator before the worker-side `StoreReader::read_record_slice()` call;
- source-record semantic queries accept immutable `source_record_index` directly and resolve through the authoritative record-index sidecar;
- `LayoutFragment.logical_id` and current logical ordinal are never reinterpreted as logical-node or physical-source locators;
- UI-lane semantic queries fail before record-index/arena disk I/O.

The source-record semantic path was admitted by PR #159, exact head
`31ed41801fca2a6b092e57274ee1f14d2deb10ee`, exact-head CI run
`34240329146` SUCCESS, merged as
`add6d43b925dc94e85111b4a57007673f142ca79`.

## Durable reopen

Logical moves publish committed order generations through `CompactArenaReader`. Newly constructed `LayoutWindowEngine` and `ZenithHotScrollSession` instances therefore recover the same committed logical permutation when they reopen the arena.

The consumer reopen oracle proves both directions:

1. LayoutWindow publishes a durable logical move;
2. a new LayoutWindow instance restores that order and reads the correct physical payload;
3. a new HotScroll instance restores the same order and uses the correct physical checkpoint/source window;
4. HotScroll publishes another durable move;
5. a subsequently opened LayoutWindow instance observes that newer generation.

This closes the gap between in-session forwarding and durable consumer authority.

## Height persistence

A moved record's height is persisted by physical source slot/block, not by current logical ordinal. Reopen reconstructs logical order from the committed permutation and combines it with the physical height state, so a moved source retains both its durable logical location and corrected physical height.

## Final audit status

The final repository-level audit found no residual production source-derived path that uses mutable logical `record_index` as an immutable physical source locator.

The audited classes include:

- ordinary `LayoutWindow` payload reads;
- checkpoint path/open/scan and checkpoint byte accounting;
- `ZenithHotScrollSession` checkpoint/raw-window caches and source reads;
- `ZenithTabRuntime` source-window prefetch publication;
- full-document export source dereference;
- compact-arena persisted height slot/block addressing;
- the admitted source-record -> logical-node semantic bridge.

Store-level loops that use a variable named `record_index` while iterating `records.idx` directly remain physical-store operations, not logical-order consumers; they do not violate the M2 identity split.

The original M2 promotion is already canonical: PR #97 exact head
`959e3b00c7b64b7fa96524905b9f81fe06a70d75`, required push run
`31796822424` SUCCESS, PR run `31797517189` SUCCESS, merge commit
`e132538834b55bb2b40157997b20693201bf6f78`.

Durable compact-arena insert/erase remain explicit non-capabilities outside this
promotion until a separate durable structural-editing protocol is admitted.
