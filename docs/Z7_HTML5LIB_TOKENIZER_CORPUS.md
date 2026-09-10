# Z7 frozen html5lib tokenizer corpus authority v2

## Purpose

The configured Z7 milestone contains an `html_tokenizer_conformance` gate. This authority pins the complete `tokenizer/*.test` fixture set present at the selected html5lib-tests commit so later execution work has a complete, immutable denominator instead of a hand-picked green subset.

Provenance success is **not** tokenizer conformance. The manifest therefore keeps `conformance_claim: false`.

## Upstream pin

- repository: `html5lib/html5lib-tests`
- commit: `224991ec10db04f056a89eed8b0bd8695fd2950e`
- license: MIT, vendored byte-for-byte at `third_party/html5lib-tests/LICENSE`
- tokenizer fixture set: **14 `.test` files**
- test objects: **6810**
- initial-state executions: **7036**

The one-shot builder requires the upstream checkout to be exactly the pinned commit and requires the complete `tokenizer/*.test` filename set, Git blob identities and byte sizes frozen in the builder before it copies anything.

`tokenizer/xmlViolation.test` is intentionally not normalized into the ordinary fixture shape: its upstream-defined `xmlViolationTests` top-level array name is separately pinned and verified. All other files use `tests`.

## Complete pinned fixture set

| Upstream fixture | Test array | Bytes | Git blob | SHA-256 | Tests | Executions |
| --- | --- | ---: | --- | --- | ---: | ---: |
| `tokenizer/contentModelFlags.test` | `tests` | 3055 | `9cf7c8bd9e70dfbd0037726d6a840e67d3aa5e12` | `77784a505a528950761cfb3c76617afade28b27c3be2a8c37dce3c3d8988391d` | 14 | 24 |
| `tokenizer/domjs.test` | `tests` | 13430 | `1a0824d789b767792286c98e92843d388eb6b5ce` | `3273e7861bbdb094571e4b0813ffdd934fe2bfd65864600fef62e8e3b807131a` | 43 | 59 |
| `tokenizer/entities.test` | `tests` | 19147 | `a6469cd0dc533d0c3b60d0ec8e8b3726524a98d7` | `fe17483810a00247579f5f129ca9c007fbab6755ba839523e29aa9f8875f4085` | 80 | 80 |
| `tokenizer/escapeFlag.test` | `tests` | 1378 | `d7d2c490bfd161681df4a846a454d588b94e3d19` | `edbd2e070a14fc67f6bbc104e50207f0fe206a21891c260deea3d227b32c93c9` | 5 | 9 |
| `tokenizer/namedEntities.test` | `tests` | 1128317 | `f74f5bff6d6513ca832da5dd12437d3f5d5861a5` | `a7f0e59ff7653820330548776cb3031c18e45f5fd1481a9813d9c7acee89bd6e` | 4210 | 4210 |
| `tokenizer/numericEntities.test` | `tests` | 49842 | `085109b797acc6657dea2ccf7c29d31942cc7214` | `679296c976252322ece27e2b113a5358a0aa3b0b8ecd2d6d9b365f9d1b0f9632` | 336 | 336 |
| `tokenizer/pendingSpecChanges.test` | `tests` | 162 | `191434f1b13532b03c1bc8519d6c4a1247ac2582` | `6b56d81ca09afa47d8cb0f33e3fb7169010c3a64493e608ebec921ac098ff8e9` | 1 | 1 |
| `tokenizer/test1.test` | `tests` | 10006 | `5323fbbeae2c6116aab14a716c8df1174e7870fb` | `524fcfa4d561a14f0c4e72e0573549abe6341fd4dfb8e16bc2dcf59a608a7219` | 69 | 69 |
| `tokenizer/test2.test` | `tests` | 8647 | `c29e4c315a66e9cb17f6629816331de4421af15a` | `f6450e77760cea823258de86f8e08894a1815671dbec0d74e7fbdab075596e37` | 45 | 45 |
| `tokenizer/test3.test` | `tests` | 349970 | `901a581e35cd64fafec2db0b4d5b2390ec12534f` | `9912fa27f03344243f1baa96d9690a5c2a4a9c9426c70da5cbf5c62391d62de4` | 1590 | 1786 |
| `tokenizer/test4.test` | `tests` | 16339 | `8963c7471184e53446afaa31e9cc7a74624a3812` | `c4967118aecbf8eb2ca34d5c5306f536614acca03e58610f75fbd9efa89fbb42` | 85 | 85 |
| `tokenizer/unicodeChars.test` | `tests` | 43771 | `49a8098528ea9771f42c619cb6a63d7bb4b6be86` | `22b7263a840da38179b13693bbfe72f0507dcd41951622456a0d3f5300ba42bd` | 323 | 323 |
| `tokenizer/unicodeCharsProblematic.test` | `tests` | 1107 | `3ddb96c011b77706b80ff98f54bc4b1288bc8cf2` | `3c166d5cfa24ee60fd7310ff0f5057e4ae0c649842ec446b5949215759e19a68` | 5 | 5 |
| `tokenizer/xmlViolation.test` | `xmlViolationTests` | 442 | `da6159e2ea7418684db258cd9fd7aad48f24af25` | `193a2f52d81adb4df4e056e3489f3bae79b3fc65253ccf31423a0e2f9c128d5c` | 4 | 4 |

## Machine authority

`config/z7_html5lib_tokenizer_corpus.json` uses schema `zevryon.z7.html5lib-tokenizer-corpus.v2` and authority `z7-html5lib-tokenizer-corpus-provenance-v2`. `scripts/z7_html5lib_tokenizer_corpus_verify.py` independently embeds the complete file mapping, test-array key, Git blob, byte-size, SHA-256, test-count and execution-count pins. Editing only the manifest therefore cannot redefine the denominator.

The verifier additionally recomputes Git blob SHA-1 from vendored bytes, validates JSON/token structure and initial-state expansion, rejects path escape, and retains its tamper/commit/blob/type self-tests.

## Execution remains separate

The existing `contentModelFlags.test` and `test1.test` runners keep their own execution claims. Adding the other twelve fixtures to provenance does not convert them into passes and does not close `html_tokenizer_conformance`.

`xmlViolation.test` is provenance-visible but its expected output is defined by html5lib's historical XML-infoset coercion convention; pinning it is not a claim that the ordinary production tokenizer should blindly reproduce that convention.

The next execution pass must run the newly pinned fixtures through production tokenizer entrypoints, report exact pass/fail/unsupported denominators, and leave unsupported states/preprocessing/recovery visible rather than filtering them out.

## Tree-builder separation

At the pinned June 26, 2026 html5lib-tests commit, tree-construction tests had moved to Web Platform Tests. Z7 tree-builder provenance therefore remains separately pinned to WPT and is not mixed into this tokenizer authority.

## Nonclaims

This authority does not claim full WHATWG tokenizer conformance, `html_tokenizer_conformance`, `tree_builder_conformance`, chunk-size equivalence, parser fuzzing, bounded-large-document completion, or Z7 completion. Z7 remains `planned` until its configured gates close with execution evidence.
