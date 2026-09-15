# Z7 frozen WPT tree-construction corpus authority

## Purpose

The configured Z7 milestone contains a `tree_builder_conformance` gate. Focused parser examples and unit tests are implementation authority; this frozen Web Platform Tests slice is the external tree-construction authority admitted for the gate.

This document defines that authority, its exact provenance, the production-backed execution path, and the claim boundary. It does not enlarge the configured corpus beyond the pinned fixture below.

## Current WPT authority

At the pinned WPT revision, `html/syntax/parsing/README` states that the `html5lib_*.html` wrappers run tree-construction `.dat` files maintained directly in `html/syntax/parsing/resources/`.

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

`config/z7_wpt_tree_corpus.json` is the machine-readable provenance authority. Its `conformance_claim: false` field is intentionally a corpus-manifest property rather than an execution result; the runtime gate owns the pass/fail claim.

## WPT `.dat` structure and browser semantics

The pinned tree-construction format contains:

1. `#data` and the exact HTML input;
2. `#errors` and expected parse-error records;
3. optional `#new-errors`;
4. optional `#document-fragment` plus context element;
5. optional `#script-off` or `#script-on`;
6. `#document` followed by the expected serialized tree.

If no scripting marker is present, the case runs in both scripting modes. The verifier and runner expand that rule when calculating the execution denominator.

At the exact pinned revision, `html/syntax/parsing/resources/test.js` explicitly treats `#errors`, `#new-errors`, and `#errors-new` as parsing-error metadata that the browser harness does not assert. The browser-facing test parses the input, serializes the resulting DOM tree, and compares that serialization with `#document`. Zevryon's configured gate follows that same criterion rather than manufacturing a parse-error-stream requirement that the pinned browser WPT harness itself does not test.

## Offline provenance verifier

`scripts/z7_wpt_tree_corpus_verify.py` contains the admitted upstream path, vendored path, Git blob, byte count, SHA-256 and cardinalities as code-level pins in addition to the manifest.

For both fixture and license it recomputes Git blob SHA-1 from vendored bytes using Git's `blob <size>\0<payload>` object framing. A manifest edit therefore cannot redefine the admitted upstream byte identity.

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

The machine-readable provenance report contains `tests_verified: 4`, `executions_verified: 8`, `provenance_gate_passed: true`, and `conformance_claim: false`.

The verifier self-test proves rejection of a one-byte fixture tamper, WPT commit drift, fixture Git-blob identity drift, and malformed WPT section ordering. Normal verification and self-test are wired into ordinary CTest when Python 3 is available and require no network access.

## Deliberately difficult authority

`adoption02.dat` exercises adoption-agency and table-related recovery rather than easy happy-path nesting. The admitted production `HtmlTreeBuilderV1` now implements the behavior required by this configured authority, including implicit `html`/`head`/`body` construction, bounded open-element and active-formatting state, adoption-agency recovery, foster parenting/table recovery, core table/select insertion modes, and text/comment/attribute materialization.

The corpus remains fixed at the same difficult four tests and eight scripting-mode executions. Passing it therefore results from production behavior changing, not from curating away difficult cases.

## Production tree-dump adapter and runner

`zevryon-html-tree-dump-v1-probe` is linked directly to `zevryon-massivedoc-core` and calls the production `build_html_tree_v1()` implementation. It contains no second HTML parser. The resulting production tree is serialized deterministically into WPT-style lines, including elements, attributes, text nodes and comments.

The probe still reports unsupported production capabilities honestly. Document fragments and namespace-aware tree serialization remain explicit general capability boundaries; neither is required by the configured `adoption02.dat` authority. Parse-error streaming is not a browser-WPT blocking capability because the pinned browser harness ignores the `.dat` error metadata.

`scripts/z7_wpt_tree_runner_v1.py`:

- re-verifies frozen WPT provenance before execution;
- expands the scripting-mode matrix without removing cases from the denominator;
- invokes the production C++ probe for every configured execution;
- preserves real production fail-closed and required-capability boundaries as unsupported executions;
- requires byte-for-byte-equivalent expected tree lines for a pass;
- reports pass/fail/unsupported totals;
- sets `tree_builder_conformance_claim: true` only when every configured execution passes with zero failures and zero unsupported executions.

`scripts/z7_wpt_tree_conformance_gate_v1.py` turns that report into a hard gate. Any failure, unsupported execution, denominator drift, provenance failure, probe failure, or tree mismatch returns non-zero.

The same authority is registered in ordinary CTest as `z7-wpt-tree-conformance-v1` and is also exercised by the focused `Z7 WPT tree conformance` GitHub Actions workflow.

## Configured conformance result and claim boundary

For the frozen v1 authority, the production gate result is:

- files: `1`;
- tests: `4`;
- executions: `8`;
- passed: `8`;
- failed: `0`;
- unsupported: `0`;
- exact-tree matches: `8`;
- `tree_builder_conformance_claim: true`.

This closes the configured Z7 `tree_builder_conformance` authority for the frozen v1 corpus. It is not a claim that every tree-construction `.dat` file in WPT passes, and it is not a claim of universal WHATWG parser conformance outside the admitted authority.

This result does not satisfy or alter `html_tokenizer_conformance`, does not change the tokenizer byte-input applicability boundary, and does not by itself change Z7 milestone status from `planned`. Those claims remain governed by their own canonical gates and milestone policy.
