# Z7 bounded HTML named character references v1

## Scope

This slice consumes the separately pinned WHATWG 2,231-entry named-character-reference table through the existing shared `consume_html_character_reference_v1()` production boundary. Numeric-reference behavior remains on the same component and is regression-tested unchanged.

Named matching is bounded by the generated 32-byte maximum entity spelling. The implementation searches candidate lengths from longest to shortest and uses binary search over the sorted generated table, so WHATWG maximum-length matching is explicit rather than approximated by a small hand-written entity list.

## Admitted behavior

The shared component now admits:

- exact semicolon-terminated named references;
- one- and two-Unicode-scalar UTF-8 replacement;
- legacy names without a semicolon with `missing-semicolon-after-character-reference`;
- attribute-context historical veto when a semicolonless match is followed by `=` or ASCII alphanumeric input;
- ambiguous-ampersand literal recovery when no table entry matches;
- `unknown-named-character-reference` when an unmatched ASCII-alphanumeric name reaches `;`.

Data and attribute callers retain their existing byte caps. Generated replacement bytes are appended under those caps before token publication.

## External test1 boundary

This production slice covers the semantics needed by ten of the eleven executions still excluded from the frozen 58/11 `tokenizer/test1.test` authority. `Non-ASCII character reference name` remains outside the current raw-input preprocessing/location authority because its input contains a non-ASCII source character.

Production capability does not itself rewrite the external-runner denominator. A separate authority-only change must measure and promote the ten named-reference executions; its expected honest target is 68 passed / 0 failed / 1 unsupported.

## Nonclaims

This slice does not admit raw non-ASCII input preprocessing, U+0000 replacement, CDATA, full WHATWG tokenizer conformance, `html_tokenizer_conformance`, tree-builder conformance, or Z7 completion. Z7 remains `planned`.
