# M1/M2 source-record to logical-node index v1

## Purpose

The existing MassiveDoc layout path is record-oriented. `LayoutFragment::logical_id` is record identity and is not a browser-node id. The authoritative logical-node arena v2 uses a separate node ordinal/id domain. This sidecar provides an explicit bridge keyed by physical `source_record_index` instead of relying on accidental integer equality.

## Publication

The create-only sidecar lives at `node-record-index-v1/` and is built through `node-record-index-v1.building/`. Any failure before final publication removes staging and leaves no authoritative index.

The format contains four files:

- `manifest.bin`: format/version, exact native-store binding, exact arena identity, source-record count, node count, posting count and total source bytes;
- `records.bin`: fixed-width absolute source prefix + physical record length entries;
- `heads.bin`: fixed-width per-record first/last posting + posting-count entries;
- `postings.bin`: append-only linked postings containing node ordinal, queried physical source-record identity and the exact overlap slice inside that record.

All fixed-width entries carry CRC32. Reader open verifies exact file sizes from the manifest before serving queries.

## Identity

The sidecar binds the same native-store authority used by `ZVNSRC01` v2 and `node-arena-v2`:

- exact physical source-record count;
- full payload SHA-256;
- stable physical record-sequence SHA-256.

It also freezes a SHA-256 identity over the authoritative arena-v2 manifest, including storage format/configuration, node/attribute/semantic counts, candidate commit/tree and the arena source identity. A stale sidecar therefore cannot be reopened against a different native store or logical-node arena.

## Cross-record semantics

A non-empty node source span is indexed into every physical record it overlaps. The builder derives each overlap from the authoritative `records.idx` lengths; it does not assume one node belongs to one record.

Each posting stores:

- logical-node ordinal;
- link to the next posting for that physical source record;
- the posting's explicit `source_record_index` owner;
- exact record-local overlap byte offset;
- exact record-local overlap byte length.

At query time the reader first requires the posting's stored source-record owner to equal the requested record. It then loads the authoritative arena node and independently recomputes the expected source overlap from the record-prefix table. A posting is rejected if its node does not actually overlap the queried record, if its stored overlap differs from the recomputed intersection, or if a continuation cursor belongs to another record chain.

Zero-length source anchors such as `#document` are deliberately not posted to a physical record.

## Boundedness

The builder streams native record descriptors and logical nodes. Per-record posting heads are updated in the staging file rather than retained in an in-memory vector proportional to record count. Posting storage grows on disk with actual node/record overlap count.

The reader uses bounded positional I/O and returns at most the caller-specified node count. `max_nodes` has a hard ceiling. A truncated result returns an opaque posting ordinal that can be supplied as the continuation cursor without rescanning earlier postings.

Linked postings always point to a strictly higher posting ordinal. The reader enforces that invariant, so cycles/back-links fail closed without retaining a visited set proportional to the chain. Posting ordinals are also range-checked against the manifest before positional reads.

The builder uses checked 64-bit positional offsets. Staging random-access handles are closed before the final directory rename so Windows publication is not dependent on unlinking/renaming open files.

## Production boundary

This sidecar alone does not close the remaining M1 browser-node plan items. Admission must be followed by a real `ZenithTabRuntime` worker-lane path that resolves layout/source-record identity through this index and then through the authoritative arena-v2 semantic consumer. UI-lane disk access remains forbidden.

Direct `LayoutFragment.logical_id -> node_by_id()` lookup remains inadmissible.
