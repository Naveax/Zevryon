# Z7 bounded HTML Data-tag tokenizer v1

## Purpose

The broader pinned html5lib tokenizer corpus defaults missing `initialStates` to Data state. Genuine expansion of `html_tokenizer_conformance` therefore requires a production Data-state token boundary rather than a runner-side approximation.

This component is a bounded Data-state tag tokenizer that emits the shared `HtmlTokenizerV1Token` representation through `HtmlTokenizerV1Sink`. Comments/DOCTYPE composition, Script-data and the canonical Data entrypoint remain separate production layers so each authority can be admitted and measured independently.

## Admitted token surface

`tokenize_html_data_tags_v1()` admits ASCII Data-state inputs containing:

- ordinary start tags;
- ordinary end tags;
- ASCII-case-insensitive tag and attribute names normalized to lowercase;
- single-quoted attribute values;
- double-quoted attribute values;
- unquoted attribute values inside the admitted byte subset;
- empty/boolean attributes;
- the self-closing start-tag flag;
- coalesced Character tokens outside tags.

The event boundary is shared with the canonical tokenizer stream. StartTag tokens carry ordered `HtmlTokenizerV1Attribute` records and the self-closing flag directly rather than being reconstructed from node output.

## Admitted recovery and parse-error authority

The component completes tokenization for the following bounded recovery families:

- `missing-whitespace-between-attributes`, retaining both admitted attributes;
- `duplicate-attribute`, retaining the first value and dropping the duplicate;
- `end-tag-with-attributes`, validating then suppressing attributes on the emitted EndTag;
- `end-tag-with-trailing-solidus`, emitting an ordinary EndTag;
- tag-open invalid ASCII `anything else`: `invalid-first-character-of-tag-name`, literal `<` Character output, then reconsume in Data;
- empty end tag `</>`: `missing-end-tag-name`, no EndTag token;
- the five special bytes `"`, `'`, `<`, `=`, and `` ` `` in an unquoted attribute value: `unexpected-character-in-unquoted-attribute-value`, with the offending byte retained in the attribute value.

Parse-error line/column positions are one-based. V1 location authority remains deliberately ASCII-only, so byte offsets and character columns are identical. Non-ASCII input remains fail-closed until preprocessing and Unicode location accounting are admitted.

## Bounds

Default caller bounds are:

- input: 1 MiB;
- token payload: 64 KiB;
- emitted attributes per tag: 256.

Fixed implementation maxima are 16 MiB input, 1 MiB token payload and 4096 attributes. Character buffering, names, values and aggregate emitted tag payload are bounded before publication.

The sink is streaming. A later unsupported construct can fail after earlier complete events were delivered. Within one tag token, publication occurs only after that token passes the admitted validation and bounds.

## Deliberate fail-closed surface

The following remain outside this component:

- `<!...` markup declarations, comments and DOCTYPE, which are owned by the separate Data-stream composition layer;
- `<?...` bogus-comment recovery;
- bogus-comment recovery for invalid end-tag-open bytes other than the admitted empty `</>` case;
- named or numeric character references in Data or attribute values;
- NUL replacement and complete input-stream preprocessing;
- non-ASCII preprocessing/location authority;
- remaining malformed tag/attribute recovery not explicitly admitted above;
- Script-data and CDATA states.

These cases return an explicit API failure instead of fabricating a token stream.

## Tree-builder feedback boundary

`<plaintext>` in Data state is emitted only as a StartTag token. This component does **not** switch itself to PLAINTEXT after emitting that token. Such state changes are driven by the tree-builder consumer in the HTML parsing algorithm. Keeping that separation is required for an honest tokenizer/tree-builder boundary.

## Focused regression authority

The established Data-tag suite covers normalized tags, attributes, duplicate/missing-whitespace diagnostics, end-tag diagnostics, `<plaintext>` separation, fail-closed declaration/reference/NUL boundaries, and hard caps.

The dedicated `html-tokenizer-data-tag-recovery-v1-tests` adds authority for:

- `<>` and a non-fixture `<1x` invalid-ASCII tag-open example;
- `</>` empty-end-tag recovery;
- all five unquoted attribute-value special bytes, including exact error position and retained payload;
- explicit proof that the new generic tag-open recovery does not accidentally admit NUL or non-ASCII preprocessing debt.

The pinned html5lib `test1.test` has three directly affected cases: `Empty end tag`, `Empty start tag`, and `Open angled bracket in unquoted attribute value state`. Their external-runner denominator must be promoted separately after this production slice is admitted. A production implementation becoming capable of a case is not itself permission to rewrite historical runner authority.

## Claim boundary

This slice does not satisfy `html_tokenizer_conformance`. It expands the production token surface required to run more of the pinned external corpus honestly.

The admitted `test1.test` runner remains a separate authority surface, and unsupported cases continue to be counted explicitly rather than converted to passes. Character references, broader recovery, preprocessing, CDATA and tree-builder conformance remain open.

`tree_builder_conformance` remains independently outstanding and Z7 remains `planned`.
