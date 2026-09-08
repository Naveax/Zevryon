# Z7 strict streaming HTML node-source v2

## Scope

This slice extends the strict streaming HTML producer onto `ZVNSRC01` v2 so ordinary HTML data-state text becomes real `#text` logical nodes. The admitted bounded RAWTEXT family now covers `style`, `xmp`, `iframe`, `noembed` and `noframes`. It does not claim full WHATWG tokenizer/tree-builder conformance.

The family boundary follows the current WHATWG parsing model: those five elements switch the tokenizer to RAWTEXT, while `title`/`textarea` use RCDATA, `script` uses script-data states, `plaintext` uses PLAINTEXT, and `noscript` depends on scripting mode. The latter states remain separate work rather than being approximated through RAWTEXT.

## Source semantics

Element and text nodes use the v2 source triple:

`(source_record_index, source_byte_offset, source_byte_length)`

`source_byte_length` is the total contiguous logical source length and may continue through following physical native-store records. Cross-record markup therefore no longer falls back to a zero-length anchor.

## Bounded text materialization

Text payload bytes are never accumulated into a parser-owned string. The producer retains only:

- starting physical record;
- starting byte offset;
- checked `uint64_t` logical byte length.

A pending text span is emitted immediately before an admitted markup transition and at end of input. Resident parser memory therefore does not scale with the length of a text node.

For an admitted RAWTEXT element, arbitrary `<` bytes remain part of the same logical text span unless they begin an ASCII-case-insensitive appropriate end-tag candidate for the active element. False candidates are folded back into the contiguous text span without materializing the payload. The candidate itself remains bounded by the configured markup-token limit and may cross StoreReader windows or physical native-store records.

The active appropriate-end-tag name is not copied into a second parser-owned string. The ledger-backed `open_elements_.back().tag` entry is the authoritative state used by the RAWTEXT matcher.

## Working-set authority

Parser-owned dynamic markup state uses the same private `ResourceLedger` plus `LedgerMemoryResource` model as the admitted v1 strict producer. Markup tokens, bounded RAWTEXT close candidates, parsed element/attribute semantics, temporary attribute views and the open-element stack are charged before allocation and released on destruction.

Hard-limit exhaustion is converted into a fail-closed parser error. Failed production cannot publish either the final v2 source or its `.building` staging file.

## Strict-profile boundary

The producer admits the bounded RAWTEXT tokenizer family for `style`, `xmp`, `iframe`, `noembed` and `noframes` only. `script`, `title`, `textarea`, `plaintext`, scripting-mode-dependent `noscript`, foreign SVG/MathML roots, processing instructions, malformed nesting and unsupported declarations remain fail-closed. Those states are later Z7 work rather than approximated semantics.

Within admitted RAWTEXT, an appropriate end-tag name is matched ASCII-case-insensitively and may be followed by ASCII whitespace before `>`. Attributes, self-closing syntax and other parse-error recovery on RAWTEXT end tags remain strict-profile rejection rather than silently inventing broader WHATWG recovery behavior.

HTML's self-closing flag is not XML element closure. The strict profile therefore rejects explicit `/>` syntax on non-void HTML elements such as `<div/>` rather than silently treating them as closed. Explicit `/>` remains accepted for genuine HTML void elements such as `<br/>`, `<img/>` and `<input/>`. Broader WHATWG parse-error recovery is still outside this slice.

## Tests

Focused authority covers:

- a start tag split across physical records;
- a text node split across physical records;
- exact parent ordinal, tag, role/style and attribute reconstruction;
- exact cross-record source triples;
- authoritative `ZVNSRC01` v2 validation against the native store;
- output equivalence across 1-, 2- and 7-byte StoreReader input windows;
- working-set hard-cap rejection and staging cleanup;
- markup-like `<` bytes inside each admitted RAWTEXT-family element as one exact text span;
- false `</iframe...>` candidates remaining part of the same text span;
- ASCII-case-insensitive `</noframes>` recognition across physical records with a 1-byte StoreReader window;
- continued fail-closed rejection of `script`, `title`, `textarea`, `plaintext` and `noscript` special tokenizer states;
- fail-closed rejection of non-void `<div/>` syntax with no published/staging sidecar;
- continued acceptance and authoritative validation of void `<br/>` syntax.

## Admission boundary

This slice emits a versioned source stream only. The isolated `node-arena-v2` representation and v2 source-to-arena importer are separate admission slices. Script-data, RCDATA, PLAINTEXT, scripting-mode handling, broader WHATWG recovery and parser fuzz/conformance breadth remain outstanding, so Z7 is not marked implemented by this change alone.
