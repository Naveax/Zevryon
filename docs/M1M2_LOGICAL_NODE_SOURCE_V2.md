# ZVNSRC01 logical-node source v2

## Status

This document defines a compatibility-preserving source-format extension. It does not replace or mutate the admitted ZVNSRC01 v1 implementation.

Version 1 remains authoritative for same-record source ranges and continues to use `LogicalNodeSourceWriter` / `LogicalNodeSourceReader` unchanged.

Version 2 is exposed through `LogicalNodeSourceV2Writer` / `LogicalNodeSourceV2Reader`.

## Why v2 exists

A browser text node is a logical semantic node, but native-store physical records are storage partitions. One text node can begin in one physical record and continue through later records.

Splitting that text node merely because a storage record ended would make DOM identity depend on storage partitioning. Representing it as a zero-length anchor would lose the actual source span.

V2 therefore defines a cross-record source span without changing the fixed binary frame layout.

## Span identity

The existing tuple is retained:

- `source_record_index`;
- `source_byte_offset`;
- `source_byte_length`.

Its v2 interpretation is:

1. `source_record_index` identifies the physical record containing the first source byte;
2. `source_byte_offset` is the byte offset inside that record;
3. `source_byte_length` is the total contiguous logical byte length;
4. if the first record ends before the length is exhausted, the span continues at offset zero of the next physical record, and so on.

A zero-length span validates only the start position and emits no payload bytes.

The physical record sequence is already bound by the dual source identity introduced in v1, including `record_sequence_sha256`. Therefore a v2 span cannot be silently attached to a differently partitioned native store.

## Binary compatibility

V2 intentionally preserves:

- magic `ZVNSRC01`;
- 112-byte source header;
- node-frame field offsets;
- attribute-frame layout;
- CRC32 protection;
- payload SHA-256 binding;
- physical record-sequence SHA-256 binding;
- node/attribute count fields.

The header format-version field is `2`.

Although the byte layout is unchanged, the source-span semantics are different. For that reason the admitted v1 reader remains strict and does not reinterpret a v2 stream. The v2 reader likewise requires version 2. This avoids an ambiguous mixed-version execution path.

## Validation authority

`validate_logical_node_source_v2_against_store()`:

1. recomputes the authoritative native-store dual binding;
2. rejects any payload, record-count or record-sequence identity mismatch;
3. requires source node count to agree with native-store `logical_nodes` metadata;
4. decodes every v2 node/frame with bounded frame and attribute counts;
5. validates every source span through `StoreReader::read_record_span()`;
6. rejects a start record or start offset outside the store;
7. rejects a span that escapes the final physical record;
8. reports validated node, attribute, streamed-byte and zero-length-span totals.

The validator does not materialize a complete source span. Payload delivery remains bounded by StoreReader's configured I/O window.

This first v2 slice intentionally performs a second bounded payload-read pass for span validation. A later descriptor-only validator may remove that I/O without changing v2 semantics.

## Publication behavior

The v2 writer follows the same create-only publication discipline as v1:

- build into `<output>.building`;
- never overwrite an authoritative output;
- publish only after the complete header and all frames are flushed successfully;
- remove unfinished build output on writer destruction.

## Current test authority

`logical-node-source-v2-tests` covers:

- a `#document` plus one `#text` node;
- a six-byte text span beginning in record 0 and continuing through records 1 and 2;
- exact node/attribute semantic round trip;
- exact streamed span-byte accounting;
- zero-length document anchor validation;
- rejection of a span escaping the final record;
- continued usability of the admitted v1 reader/writer;
- rejection of a v1 header by the v2 reader.

The StoreReader cross-record primitive itself has separate tests for chunk/window-size equivalence and early consumer termination.

## Admission boundary

This slice does not change `ZVNODA01` arena semantics and does not switch the HTML producer to v2 yet.

The next coherent steps are:

1. admit a versioned arena representation for cross-record source spans;
2. switch the strict HTML producer to emit normal data-state text nodes through v2;
3. import those v2 nodes into the versioned arena;
4. continue toward raw-text/RCDATA tokenizer states and WHATWG tree-building conformance;
5. connect a bounded production consumer of the logical-node arena.

Z7 remains planned and issue #145 remains open until those larger production conditions are satisfied.
