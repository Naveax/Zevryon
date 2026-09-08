# Z7 strict streaming HTML node-source v2

## Scope

This slice extends the strict streaming HTML producer onto `ZVNSRC01` v2 so ordinary HTML data-state text becomes real `#text` logical nodes. It does not claim full WHATWG tokenizer/tree-builder conformance.

## Source semantics

Element and text nodes use the v2 source triple:

`(source_record_index, source_byte_offset, source_byte_length)`

`source_byte_length` is the total contiguous logical source length and may continue through following physical native-store records. Cross-record markup therefore no longer falls back to a zero-length anchor.

## Bounded text materialization

Text payload bytes are never accumulated into a parser-owned string. The producer retains only:

- starting physical record;
- starting byte offset;
- checked `uint64_t` logical byte length.

A pending text span is emitted immediately before the next markup token and at end of input. Resident parser memory therefore does not scale with the length of a text node.

## Working-set authority

Parser-owned dynamic markup state uses the same private `ResourceLedger` plus `LedgerMemoryResource` model as the admitted v1 strict producer. Markup tokens, parsed element/attribute semantics, temporary attribute views and the open-element stack are charged before allocation and released on destruction.

Hard-limit exhaustion is converted into a fail-closed parser error. Failed production cannot publish either the final v2 source or its `.building` staging file.

## Strict-profile boundary

The producer still deliberately rejects unsupported semantics including raw-text/RCDATA families such as `script`, `style`, `title` and `textarea`, foreign SVG/MathML roots, processing instructions, malformed nesting and unsupported declarations. Those states remain later Z7 work rather than being approximated.

## Tests

Focused authority covers:

- a start tag split across physical records;
- a text node split across physical records;
- exact parent ordinal, tag, role/style and attribute reconstruction;
- exact cross-record source triples;
- authoritative `ZVNSRC01` v2 validation against the native store;
- output equivalence across 1-, 2- and 7-byte StoreReader input windows;
- working-set hard-cap rejection and staging cleanup;
- continued fail-closed rejection of unsupported raw-text input.

## Admission boundary

This slice emits a versioned source stream only. The isolated `node-arena-v2` representation and v2 source-to-arena importer are separate admission slices. Full raw-text/RCDATA handling, WHATWG recovery and broader parser fuzz/conformance remain outstanding, so Z7 is not marked implemented by this change alone.
