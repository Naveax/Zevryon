# Z7 streaming HTML parser milestone certification

## Status

`config/zenith_program.json` marks Z7 `implemented` only after all five configured gates have canonical passing authority.

| Required gate | Canonical authority | Result |
|---|---|---|
| `html_tokenizer_conformance` | `scripts/z7_html5lib_tokenizer_byte_conformance_v1.py` | 7,028/7,028 applicable production UTF-8 byte-input executions pass; 0 applicable failures; 8 frozen out-of-scope abstract/Infoset executions |
| `tree_builder_conformance` | `scripts/z7_wpt_tree_conformance_gate_v1.py` | 8/8 configured WPT executions exact-tree match; 0 failed; 0 unsupported |
| `chunk_size_equivalence` | `zevryon-streaming-html-chunk-size-equivalence-tests` | deterministic production output/failure equivalence across 16 input-window sizes on Linux and Windows |
| `parser_fuzzing` | `zevryon-streaming-html-parser-fuzz-certification-tests` | deterministic 10,000-case sanitizer authority under ASan/UBSan |
| `bounded_large_document_parse` | `zevryon-streaming-html-large-document-certification-tests` | bounded 64 MiB Data/RAWTEXT parses plus oversized malformed-token bounded rejection |

## Immutable CI evidence

- tokenizer gate: Actions run `34864693236`, admitted by merge `c402ad4c2015cdd06df91a04ecab0c9e00dbaa4b`;
- chunk-size equivalence: Actions run `34864744881`, admitted by merge `36b1b0382a1f8343eeed1968ed02dc96fe9509a3`;
- parser fuzzing and bounded-large parsing: Actions run `34949079773`, admitted by merge `4a6aa174027961653a89323ec9b34266a9f69ce9`;
- configured WPT tree gate: Actions run `34958001841`, admitted by merge `99f716732aecef2603071b3e4239a1e7dfeb4f59`.

The certification targets are repository-owned CMake targets. Focused workflows checkout the exact pull-request head instead of synthesizing targets in CI.

## Claim boundary

Z7 completion is the completion of the five gates configured in `config/zenith_program.json`; it is not a claim of universal HTML-platform conformance beyond those admitted authorities.

Tokenizer conformance is explicitly scoped to production-valid UTF-8 byte input. The four isolated-surrogate html5lib abstract-input cases and four XML Infoset-coercion cases remain frozen outside that production input domain and are not relabeled as passes.

Tree-builder conformance is explicitly scoped to the frozen configured WPT v1 authority. Document-fragment and namespace-aware serialization remain outside that configured corpus unless separately admitted later.
