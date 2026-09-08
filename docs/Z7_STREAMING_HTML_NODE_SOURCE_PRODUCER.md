# Z7 streaming HTML node-source producer, strict slice v1

## Status

This is a real parser/import producer slice, not a claim that Z7 is implemented.

`config/zenith_program.json` keeps Z7 `planned` until the full required gate set exists: HTML tokenizer conformance, tree-builder conformance, chunk-size equivalence, parser fuzzing, and bounded large-document parsing.

The purpose of this slice is narrower: connect authoritative native-store HTML bytes to the `ZVNSRC01` semantic-node source without inventing semantics from envelope counts.

## Production path

`produce_streaming_html_node_source()`:

1. verifies the native store and computes the dual `ZVNSRC01` store binding;
2. opens `StoreReader` with a caller-bounded I/O window;
3. emits one real `#document` logical node;
4. tokenizes markup incrementally across StoreReader callback chunks and physical record boundaries;
5. emits element tags, attributes, decoded role/style values, parent ordinals, and source anchors through `LogicalNodeSourceWriter`;
6. verifies exact end-tag nesting in the strict profile;
7. requires the produced node count to equal native-store `logical_nodes` metadata;
8. publishes the create-only sidecar only after the whole parse succeeds.

The parser does not use `logical_nodes` to decide what nodes exist. It compares the final real parse count to that field only after parsing.

## Bounded state

Default limits are:

- StoreReader input window: 64 KiB;
- current markup/comment token: 1 MiB;
- attributes per element: 4,096;
- open element depth: 4,096.

Hard configuration ceilings are 16 MiB per token, 65,536 attributes, and 65,536 open elements.

Resident parser state is therefore independent of total document node count. The open-element stack is depth-bounded, the current token is byte-bounded, and per-token attributes are count-bounded. Completed nodes are streamed directly into the disk-backed source writer.

## Source anchors

A start tag that begins and ends inside one physical record stores its exact record-local start-tag byte range.

A start tag may cross a physical record boundary because StoreReader records are storage units rather than tokenizer boundaries. `ZVNODA01` v1 cannot encode a multi-record range, so the producer records a zero-length anchor at the exact physical record/offset where the `<` byte began. The sidecar still remains protected by both payload and physical record-sequence digests.

No cross-record bytes are falsely claimed to belong to one record.

## Semantics implemented in this slice

- ASCII case-folded element and attribute names;
- start/end tags;
- exact nested element topology;
- standard HTML void-element list;
- explicit self-closing syntax in the strict profile;
- quoted, unquoted, and boolean attributes;
- duplicate-attribute rejection after ASCII case folding;
- `role` and `style` extraction while preserving the original attribute entries;
- five basic named character references: `amp`, `lt`, `gt`, `quot`, `apos`;
- decimal and hexadecimal numeric Unicode scalar references;
- HTML comments;
- strict `<!doctype html>`.

Text bytes are intentionally not materialized as logical text nodes in this first slice. They remain authoritative in the native store and are ignored by the element-tree producer. This means this slice alone is insufficient for a complete DOM and cannot satisfy Z8.

## Explicit fail-closed boundary

The strict profile rejects semantics that would otherwise be easy to misparse:

- `script`, `style`, `title`, `textarea`, `xmp`, `iframe`, `noembed`, `noframes`, and `plaintext` raw-text states;
- SVG and MathML foreign-content roots;
- mismatched or unclosed non-void elements;
- processing instructions and unsupported declarations;
- legacy/public doctypes;
- duplicate attributes;
- malformed/unsupported character references;
- token, depth, or attribute budget violations;
- final node-count disagreement with native-store metadata.

The next Z7 slices should add proper tokenizer states and WHATWG tree-building behavior instead of weakening these rejections into silent approximations.

## Current deterministic tests

The focused test target covers:

- a real doctype/comment/element stream split across physical records;
- role/style character-reference decoding;
- attribute preservation;
- cross-record start-tag anchor behavior;
- source-to-`ZVNODA01` import and reopened topology/semantic resolution;
- byte-identical `ZVNSRC01` output with 1-byte versus 257-byte StoreReader windows;
- mismatched-end-tag rejection and `.building` cleanup;
- raw-text rejection;
- open-element depth enforcement;
- markup-token byte enforcement;
- case-folded duplicate-attribute rejection;
- `logical_nodes` envelope mismatch rejection.

## Remaining Z7 / #145 boundary

This producer is meaningful progress toward issue #145 because actual HTML bytes now generate explicit semantic events. It still does not close #145:

- text nodes and complete browser-node semantics are not produced yet;
- the full WHATWG tokenizer/tree builder is not implemented;
- a bounded production layout/accessibility/browser consumer has not yet switched to `LogicalNodeArenaReader`.

Likewise Z7 must remain `planned`; no milestone status or evidence claim is changed by this slice.
