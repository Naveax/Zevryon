# Z7 frozen WPT tree-construction corpus authority

## Purpose

The configured Z7 milestone contains a `tree_builder_conformance` gate. The strict streaming parser's focused examples and source-span tests are implementation authority, not an external tree-construction conformance suite.

This slice establishes the first frozen Web Platform Tests tree-construction corpus boundary with exact provenance and offline verification. It intentionally does **not** claim that Zevryon's current strict tree builder passes the selected WPT cases.

## Current WPT authority

At the pinned WPT revision, `html/syntax/parsing/README` states that the `html5lib_*.html` wrappers run tree-construction `.dat` files maintained directly in `html/syntax/parsing/resources/`. This matches the June 2026 html5lib-tests change that moved tree-construction authority to WPT.

The v1 tree corpus is pinned to:

- repository: `web-platform-tests/wpt`;
- commit: `fadb01bc53cd4a9fac9352c977df1787425b856e`;
- upstream fixture: `html/syntax/parsing/resources/adoption02.dat`;
- upstream fixture Git blob: `880cb505a660efdcacd287d50702d6b68a1c94d7`;
- fixture bytes: `1343`;
- fixture SHA-256: `e091e6976f861ae616fe56c527a78e7247ee7562bcafec996d4e4bda657bd9b7`;
- tree test blocks: `4`;
- expected parse-error records: `17`;
- scripting-mode executions: `8`.

None of the four selected cases carries `#script-off` or `#script-on`. Per the WPT/html5lib tree-test format, each therefore represents two executions, one with scripting disabled and one with scripting enabled.

The WPT 3-Clause BSD license is vendored byte-for-byte at `third_party/wpt/LICENSE.md`; its Git blob is `39c46d03ac2988226f949ee7ab3c7347d5481bd8`, byte size is `1500`, and SHA-256 is `5fac07febb0e2a97fb0d7b0def149ec08b642e1ba4b9c345283ab1cbd2af6570`.

`config/z7_wpt_tree_corpus.json` is the machine-readable authority and explicitly contains `conformance_claim: false`.

## WPT `.dat` structure

The pinned WPT tree-construction format requires each test to contain, in order:

1. `#data` and the exact HTML input;
2. `#errors` and expected parse-error records;
3. optional `#new-errors`;
4. optional `#document-fragment` plus context element;
5. optional `#script-off` or `#script-on`;
6. `#document` followed by the expected tree dump.

If no scripting marker is present, the case must run in both scripting modes. The v1 verifier expands that rule when calculating execution cardinality instead of treating one `.dat` block as one run.

## Offline provenance verifier

`scripts/z7_wpt_tree_corpus_verify.py` contains the admitted upstream path, vendored path, Git blob, byte count, SHA-256 and cardinalities as code-level pins in addition to the manifest.

For both the fixture and license it recomputes Git blob SHA-1 from the vendored bytes using Git's `blob <size>\0<payload>` object framing. A manifest edit therefore cannot redefine the admitted upstream byte identity.

The verifier requires:

- exact manifest schema and authority;
- exact WPT repository and commit pin;
- exact v1 fixture set and path mapping;
- byte count, SHA-256 and recomputed Git blob identity;
- LF-only fixture formatting and final LF;
- four non-empty tree-test blocks;
- valid section ordering;
- document dumps whose lines use the WPT `| ` tree-dump prefix;
- exactly `17` expected parse-error lines;
- exactly `8` scripting-mode executions.

The machine-readable success report contains `tests_verified: 4`, `executions_verified: 8`, `provenance_gate_passed: true`, and `conformance_claim: false`.

The `--self-test` path proves rejection of:

- a one-byte fixture tamper;
- WPT commit drift;
- fixture Git-blob identity drift;
- malformed WPT section ordering.

Both normal verification and self-test are wired into ordinary CTest when Python 3 is available. They require no network access.

## Why this fixture is deliberately difficult

`adoption02.dat` exercises adoption-agency and table-related tree-building behavior. The current strict Zevryon parser does not yet implement the full WHATWG adoption-agency algorithm, insertion-mode recovery, implicit html/head/body construction, scripting-mode matrix, or the complete parse-error model.

That is useful for authority: the external corpus is allowed to expose unsupported behavior. The corpus must not be curated to contain only cases the current parser already happens to pass.

## Production tree-dump adapter and runner v1

`zevryon-html-tree-dump-v1-probe` is linked directly to `zevryon-massivedoc-core` and drives `produce_streaming_html_node_source()`. It does not contain a second HTML parser. Because the production parser requires native-store `logical_nodes` envelope agreement, the adapter performs a bounded discovery pass, accepts only the parser's exact envelope-mismatch sentinel as node-count discovery, rebuilds an exact-count store, then re-runs the same production parser and reads the published `ZVNSRC01` node stream. Any other production rejection remains explicit `UNSUPPORTED`.

The probe serializes the currently materialized element/attribute topology into deterministic WPT-style tree lines. Its capability record explicitly keeps parse-error streams, document fragments, text nodes, comment nodes and namespaces disabled until those production surfaces exist.

`scripts/z7_wpt_tree_runner_v1.py`:

- re-verifies the frozen WPT provenance before execution;
- expands the WPT scripting-mode matrix without removing cases from the denominator;
- invokes the production C++ probe for every non-fragment execution;
- distinguishes production fail-closed results, known capability boundaries and actual tree mismatches;
- reports explicit pass/fail/unsupported totals while keeping `tree_builder_conformance_claim: false`.

On the current difficult `adoption02.dat` authority the runner therefore provides a measurement surface, not a conformance shortcut. The configured eight executions remain in the denominator even when the strict production tree builder rejects adoption-agency/table recovery that it does not implement.

## Admission boundary

This slice establishes reproducible WPT tree-corpus provenance plus a production-backed execution/measurement boundary. It does not satisfy `tree_builder_conformance`, does not satisfy `html_tokenizer_conformance`, and does not change Z7 from `planned`.

The next tree-conformance work is to replace explicit runner capability boundaries and production fail-closed recovery classes with real WHATWG tree-builder behavior, while preserving the same frozen denominator and production-probe boundary.
