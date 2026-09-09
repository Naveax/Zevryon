# Z7 strict streaming HTML node-source v2

## Scope

This slice extends the strict streaming HTML producer onto `ZVNSRC01` v2 so ordinary HTML data-state text becomes real `#text` logical nodes. The admitted bounded RAWTEXT family covers `style`, `xmp`, `iframe`, `noembed` and `noframes`. Structural RCDATA node/source-span handling additionally covers `title` and `textarea`. PLAINTEXT source-span handling covers `<plaintext>` through end of input for non-NUL bytes. It does not claim full WHATWG tokenizer/tree-builder conformance.

The tokenizer-state boundary follows the current WHATWG parsing model: the five RAWTEXT elements use RAWTEXT, `title`/`textarea` use RCDATA, `script` uses script-data states, `plaintext` switches permanently to PLAINTEXT until EOF, and `noscript` depends on scripting mode. Unsupported states remain separate work rather than being approximated through a neighboring state.

## Source semantics

Element and text nodes use the v2 source triple:

`(source_record_index, source_byte_offset, source_byte_length)`

`source_byte_length` is the total contiguous logical source length and may continue through following physical native-store records. Cross-record markup therefore no longer falls back to a zero-length anchor.

This source format stores authoritative raw source identity, not a decoded browser text payload. RCDATA character-reference bytes such as `&amp;` therefore remain inside the raw `#text` source span. A later decoded-text/DOM consumer must apply the tokenizer's character-reference semantics without changing the source identity established here.

## Bounded text materialization

Text payload bytes are never accumulated into a parser-owned string. The producer retains only:

- starting physical record;
- starting byte offset;
- checked `uint64_t` logical byte length.

A pending text span is emitted immediately before an admitted markup transition and at end of input. Resident parser memory therefore does not scale with the length of a text node.

For admitted RAWTEXT or RCDATA elements, arbitrary markup-like `<` bytes remain part of the same logical text span unless they begin an ASCII-case-insensitive appropriate end-tag candidate for the active element. False candidates are folded back into the contiguous text span without materializing the payload. The candidate itself remains bounded by the configured markup-token limit and may cross StoreReader windows or physical native-store records.

The active appropriate-end-tag name is not copied into a second parser-owned string. The ledger-backed `open_elements_.back().tag` entry remains authoritative for both RAWTEXT and RCDATA matching.

## PLAINTEXT through EOF

After an admitted `<plaintext>` start tag, every remaining non-NUL input byte is incorporated into the same ordinary text-span machinery until EOF. There is no appropriate-end-tag transition out of the state. Bytes such as `<`, `&`, `<b>` and the apparent `</plaintext>` spelling are therefore raw text-source bytes rather than markup or character-reference entry points.

EOF while PLAINTEXT is active is a normal admitted termination. The strict profile does not require a synthetic closing tag before publication because the tokenizer cannot emit one after entering PLAINTEXT.

WHATWG PLAINTEXT processing replaces U+0000 with U+FFFD. `ZVNSRC01` v2 currently carries authoritative raw source spans rather than a decoded replacement-character payload, so an input NUL in PLAINTEXT fails closed with an explicit error. This avoids claiming replacement semantics that the representation cannot yet encode.

## Textarea leading newline

HTML parsing ignores one LF token immediately after a `textarea` start tag. The v2 source producer therefore excludes an immediately following literal `\n` byte from the textarea browser text-node source span.

The native store currently exposes original bytes rather than a preprocessing-normalized HTML input stream. Correct CR and CRLF handling therefore requires an explicit preprocessing contract. Until that contract exists, an initial `\r` after `<textarea>` fails closed rather than silently pretending to implement CR/CRLF normalization.

## Working-set authority

Parser-owned dynamic markup state uses the same private `ResourceLedger` plus `LedgerMemoryResource` model as the admitted v1 strict producer. Markup tokens, bounded text-state close candidates, parsed element/attribute semantics, temporary attribute views and the open-element stack are charged before allocation and released on destruction.

Hard-limit exhaustion is converted into a fail-closed parser error. Failed production cannot publish either the final v2 source or its `.building` staging file.

## Strict-profile boundary

The producer admits:

- normal data-state text-node source spans;
- RAWTEXT structure for `style`, `xmp`, `iframe`, `noembed`, `noframes`;
- structural RCDATA source spans for `title` and `textarea`;
- the literal-LF textarea start rule;
- PLAINTEXT for all remaining non-NUL input bytes through EOF.

`script`, scripting-mode-dependent `noscript`, PLAINTEXT NUL replacement, CR/CRLF preprocessing, decoded RCDATA text payload semantics, foreign SVG/MathML roots, processing instructions, malformed nesting and unsupported declarations remain fail-closed or outside this representation. Those states are later Z7 work rather than approximated semantics.

Within admitted RAWTEXT/RCDATA structure, an appropriate end-tag name is matched ASCII-case-insensitively and may be followed by ASCII whitespace before `>`. Attributes, self-closing syntax and other parse-error recovery on those end tags remain strict-profile rejection rather than silently inventing broader WHATWG recovery behavior.

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
- false `</iframe...>` RAWTEXT candidates remaining part of the same text span;
- ASCII-case-insensitive `</noframes>` RAWTEXT recognition across physical records with a 1-byte StoreReader window;
- `title` RCDATA preserving character-reference and markup-like bytes in one authoritative raw source span;
- false `</title...>` RCDATA candidates remaining text;
- mixed-case `</title>` recognition across physical records with a 1-byte input window;
- omission of one literal LF immediately after `<textarea>` from the text-node source span;
- fail-closed initial textarea CR behavior until preprocessing is implemented;
- PLAINTEXT consuming apparent markup, `&amp;` spelling and apparent `</plaintext>` through EOF as one raw text span;
- PLAINTEXT source identity across physical records with a 1-byte StoreReader window;
- explicit fail-closed PLAINTEXT NUL behavior with no published or staging sidecar;
- continued fail-closed rejection of `script` and `noscript` special tokenizer states;
- fail-closed rejection of non-void `<div/>` syntax with no published/staging sidecar;
- continued acceptance and authoritative validation of void `<br/>` syntax.

## Admission boundary

This remains a strict incremental source-stream slice. Script-data, decoded RCDATA/PLAINTEXT replacement payload semantics, complete HTML input preprocessing, scripting-mode handling, broader WHATWG recovery/tree building, foreign content and final parser fuzz/conformance/large-document certification remain outstanding. Z7 therefore remains `planned`.
