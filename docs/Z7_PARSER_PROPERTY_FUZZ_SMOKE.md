# Z7 parser property-fuzz smoke

## Purpose

This authority adds a deterministic Windows/Linux CTest smoke for the strict streaming HTML v2 parser. It targets two Z7 correctness dimensions that ordinary hand-written examples only sample narrowly:

- StoreReader input-window / chunk-size equivalence;
- deterministic fail-closed behavior under fragmented input.

It is an implementation smoke, not final `parser_fuzzing` certification and not full WHATWG tokenizer/tree-builder conformance.

## Deterministic identity

The smoke uses the fixed root seed:

`0x5a375f48544d4c32`

Each of 64 cases receives a deterministic `splitmix64`-derived case seed. A failure prints the exact zero-based case index and derived seed so the same generated document and physical-record segmentation can be reproduced after a candidate change.

## Generated successful domains

Successful cases rotate through:

- ordinary data-state text with decoded start-tag attribute references;
- nested admitted elements with multiple text-node boundaries;
- `style` RAWTEXT with markup-like bytes and false appropriate-end-tag candidates;
- `xmp` / `iframe` / `noembed` / `noframes` RAWTEXT-family cases;
- `title` structural RCDATA with raw character-reference bytes and false end-tag candidates;
- `textarea` structural RCDATA including the admitted leading literal-LF rule.

Each generated HTML document is split into deterministic random physical native-store records before parsing. The same exact store is then parsed with StoreReader windows of 1, 2, 3, 5, 7, 16 and 64 bytes.

For every successful case the smoke requires:

1. every window parses successfully;
2. the resulting `ZVNSRC01` v2 artifacts are byte-identical across all windows;
3. every artifact validates against the exact native store;
4. validated node count matches the generator's independent node-count oracle;
5. parser working-set accounting returns to zero with no accounting errors;
6. peak parser working set remains within its configured hard limit.

## Generated rejection domains

Rejected cases include:

- unimplemented `script` tokenizer state;
- non-void self-closing syntax;
- mismatched strict-profile end tags.

For every rejected case the smoke requires every StoreReader window to:

- reject rather than publish a source;
- return the same exact failure string;
- include the generator's expected failure-class fragment;
- leave no final or `.building` source artifact;
- release parser working-set accounting cleanly.

This makes chunking an implementation detail rather than a source of success/failure or diagnostic nondeterminism.

## Admission boundary

The smoke materially strengthens Z7 `chunk_size_equivalence` and parser-fuzz preparation, but does not close either milestone gate by itself. The parser now separately admits non-NUL PLAINTEXT through EOF, but this original 64-case smoke does not yet generate PLAINTEXT cases. It also does not yet provide:

- a certification-scale case count or external evidence bundle;
- broad malformed-byte/NUL/input-preprocessing coverage;
- script-data or scripting-mode-dependent `noscript` semantics;
- full named-character-reference semantics;
- WHATWG tree-builder recovery and foreign-content behavior;
- coverage-guided fuzzing or sanitizer-backed long-running fuzz evidence;
- final bounded large-document parser certification.

Z7 therefore remains `planned` after this slice.
