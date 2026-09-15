# Z7 WPT browser tree-runner semantics

## Authority

The frozen tree corpus remains pinned to WPT commit `fadb01bc53cd4a9fac9352c977df1787425b856e`.

At that exact revision, `html/syntax/parsing/resources/test.js` documents the `.dat` error sections as parsing-error metadata and explicitly ignores `#errors`, `#new-errors`, and `#errors-new` when running browser tests. The browser harness parses the input, serializes the resulting DOM tree, and asserts the serialized tree against `#document`.

Zevryon's production runner therefore uses the same browser-facing criterion:

- every configured execution remains in the denominator;
- production fail-closed results remain unsupported;
- missing fragment, text, comment, or namespace tree surfaces remain explicit capability boundaries when a fixture requires them;
- `.dat` parse-error records remain preserved and provenance-verified metadata, but do not manufacture a browser conformance requirement that the pinned WPT harness itself does not assert;
- an execution passes only when the production tree dump exactly matches the frozen `#document` tree.

`tree_builder_conformance_claim` is true only when every configured execution passes with zero failures and zero unsupported executions. This does not enlarge the configured corpus and does not change Z7 milestone status by itself.
