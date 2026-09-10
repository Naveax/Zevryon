# Z7 Data tag recovery v1

## Scope

This slice admits three bounded WHATWG tokenizer recovery families inside the existing production Data-tag component. It does not add fixture-specific branches.

The admitted behavior is:

1. **Tag-open invalid ASCII byte**: emit `invalid-first-character-of-tag-name`, append the literal `<` to Character output, and reconsume the current ASCII byte in Data state.
2. **Empty end tag `</>`**: emit `missing-end-tag-name`, discard the empty end-tag spelling, and continue in Data state.
3. **Unquoted attribute special bytes**: for `"`, `'`, `<`, `=`, and `` ` ``, emit `unexpected-character-in-unquoted-attribute-value` and append the byte to the current attribute value.

These are the state-machine behaviors required by the current HTML Standard for tag-open, end-tag-open, and unquoted attribute-value handling.

## Bounded / fail-closed invariants

The new recovery does not weaken the existing preprocessing boundary:

- U+0000 remains fail-closed until replacement/preprocessing semantics are admitted;
- non-ASCII bytes remain fail-closed where the v1 component lacks preprocessing/location authority;
- character references remain outside this slice;
- bogus-comment recovery after an invalid end-tag-open byte other than `>` remains outside this slice;
- existing token-byte and attribute-count bounds remain unchanged.

The tag-open recovery therefore checks NUL and non-ASCII debt before applying the generic ASCII `anything else` behavior.

## Regression authority

`html-tokenizer-data-tag-recovery-v1-tests` covers:

- `<>` → Character `<>` plus `invalid-first-character-of-tag-name` at line 1 / column 2;
- a non-fixture numeric tag-open example (`<1x`) to prove the implementation is general state recovery rather than a special-case for `<>`;
- `</>` → no token plus `missing-end-tag-name` at line 1 / column 3;
- all five special bytes in unquoted attribute values, each retained in the attribute payload with the exact parse error at line 1 / column 7;
- NUL and non-ASCII tag-open inputs remaining fail-closed with no recovery events published.

The separately pinned html5lib `test1.test` contains three directly affected cases: `Empty end tag`, `Empty start tag`, and `Open angled bracket in unquoted attribute value state`. This implementation slice does not itself change the admitted external-runner denominator. A later runner-denominator slice must explicitly promote those cases only after this production slice is admitted.

## Explicit nonclaims

This slice does not claim:

- complete tag-open/end-tag-open recovery;
- bogus-comment recovery for arbitrary invalid end-tag-open bytes;
- complete attribute-state recovery;
- character-reference handling;
- full input preprocessing/U+0000 replacement;
- general non-ASCII tokenizer authority;
- `test1.test` 69/69;
- `html_tokenizer_conformance` closure;
- `tree_builder_conformance` closure;
- Z7 completion.

Z7 remains `planned`.
