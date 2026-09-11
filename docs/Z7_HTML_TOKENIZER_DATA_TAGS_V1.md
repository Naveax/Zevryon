# Z7 bounded HTML Data-tag tokenizer v1

## Purpose

The broader pinned html5lib tokenizer corpus defaults missing `initialStates` to Data state. Genuine expansion of `html_tokenizer_conformance` therefore requires a production Data-state token boundary rather than a runner-side approximation.

This component is a bounded Data-state tag tokenizer that emits the shared `HtmlTokenizerV1Token` representation through `HtmlTokenizerV1Sink`. Comments/DOCTYPE composition, Script-data and the canonical Data entrypoint remain separate production layers so each authority can be admitted and measured independently.

## Admitted token surface

`tokenize_html_data_tags_v1()` admits ASCII raw Data-state inputs containing:

- ordinary start tags;
- ordinary end tags;
- bounded ASCII tag-name and attribute-name state handling, with ASCII letters normalized to lowercase;
- single-quoted attribute values;
- double-quoted attribute values;
- unquoted attribute values inside the admitted byte subset;
- empty/boolean attributes;
- the self-closing start-tag flag;
- coalesced Character tokens outside tags;
- bounded bogus Comment tokens entered from `<?...` tag-open recovery and invalid end-tag-open bytes;
- literal ampersand fallback;
- decimal and hexadecimal numeric character references in Data and attribute values;
- the complete pinned WHATWG named-character-reference table in Data and attribute values.

Numeric references may decode to multi-byte UTF-8 output even though raw input remains ASCII-only for the current preprocessing/location authority. Decoded replacement bytes are appended under the existing token/attribute byte bound.

The event boundary is shared with the canonical tokenizer stream. StartTag tokens carry ordered `HtmlTokenizerV1Attribute` records and the self-closing flag directly rather than being reconstructed from node output.

## Admitted recovery and parse-error authority

The component completes tokenization for the following bounded recovery families:

- `missing-whitespace-between-attributes`, retaining both admitted attributes;
- before-attribute-name `=` recovery: `unexpected-equals-sign-before-attribute-name`, creating an attribute whose initial name byte is `=`;
- attribute-name `"`, `'` and `<`: `unexpected-character-in-attribute-name`, with the offending ASCII byte retained;
- admitted C0/DEL attribute-name input controls: `control-character-in-input-stream`, preserving the byte and maintaining input-error ordering ahead of tokenizer recovery on the same byte;
- `missing-attribute-value` when an equals delimiter reaches `>` before a value;
- ordinary admitted ASCII punctuation in attribute names, while `/`, whitespace, `>` and `=` retain their tokenizer transition roles;
- `duplicate-attribute`, retaining the first value and dropping the duplicate;
- `end-tag-with-attributes`, validating then suppressing attributes on the emitted EndTag;
- `end-tag-with-trailing-solidus`, emitting an ordinary EndTag;
- ASCII tag-name `anything else` bytes are retained in the normalized tag name, while admitted C0/DEL controls emit `control-character-in-input-stream`;
- self-closing-start-tag recovery: a `/` not followed by `>` emits `unexpected-solidus-in-tag` and reconsumes the following admitted ASCII byte in before-attribute-name, preserving input-stream error ordering;
- EOF after tag-open or end-tag-open: `eof-before-tag-name`, with the literal `<` or `</` bytes emitted as Character data;
- EOF in tag-name, attribute-name/value, after-attribute and self-closing-start-tag states: `eof-in-tag`, discarding the incomplete tag token without publishing partial attributes;
- tag-open invalid ASCII `anything else`: `invalid-first-character-of-tag-name`, literal `<` Character output, then reconsume in Data;
- empty end tag `</>`: `missing-end-tag-name`, no EndTag token;
- `<?...` tag-open recovery: `unexpected-question-mark-instead-of-tag-name`, reconsuming `?` into a bounded bogus Comment token;
- invalid ASCII end-tag-open recovery such as `</1>`: `invalid-first-character-of-tag-name`, reconsuming the offending byte into a bounded bogus Comment token;
- bogus Comment data stops at the first `>` or EOF, preserves admitted UTF-8 scalar bytes, reports admitted input controls and obeys the token byte cap;
- the five special bytes `"`, `'`, `<`, `=`, and `` ` `` in an unquoted attribute value: `unexpected-character-in-unquoted-attribute-value`, with the offending byte retained in the attribute value;
- digitless numeric references: `absence-of-digits-in-numeric-character-reference` plus literal temporary-buffer recovery;
- numeric references without `;`: `missing-semicolon-after-character-reference` with the terminating byte reconsumed by the caller;
- numeric end-state diagnostics for null, out-of-range, surrogate, noncharacter and control references, including the WHATWG C1 replacement table.

Parse-error line/column positions are one-based. V1 raw-input location authority remains deliberately ASCII-only, so source byte offsets and source character columns are identical. Non-ASCII raw input remains fail-closed until preprocessing and Unicode location accounting are admitted.

## Bounds

Default caller bounds are:

- input: 1 MiB;
- token payload: 64 KiB;
- emitted attributes per tag: 256.

Fixed implementation maxima are 16 MiB input, 1 MiB token payload and 4096 attributes. Character buffering, names, values, decoded reference replacements and aggregate emitted tag payload are bounded before publication.

The sink is streaming. A later unsupported construct can fail after earlier complete events were delivered. Within one tag token, publication occurs only after that token passes the admitted validation and bounds.

## Deliberate fail-closed surface

The following remain outside this component:

- `<!...` markup declarations, comments and DOCTYPE, which are owned by the separate Data-stream composition layer;
- NUL replacement and complete input-stream preprocessing;
- non-ASCII raw-input preprocessing/location authority, including non-ASCII tag and attribute names;
- remaining malformed tag/attribute recovery not explicitly admitted above;
- Script-data and CDATA states.

These cases return an explicit API failure instead of fabricating a token stream.

## Tree-builder feedback boundary

`<plaintext>` in Data state is emitted only as a StartTag token. This component does **not** switch itself to PLAINTEXT after emitting that token. Such state changes are driven by the tree-builder consumer in the HTML parsing algorithm. Keeping that separation is required for an honest tokenizer/tree-builder boundary.

## Focused regression authority

The established Data-tag suite covers normalized tags, attributes, duplicate/missing-whitespace diagnostics, end-tag diagnostics, `<plaintext>` separation and hard caps. It also covers bogus-comment entry from `<?...` and invalid end-tag-open bytes, EOF termination, event ordering, control diagnostics and the comment token byte cap.

The dedicated `html-tokenizer-data-tag-recovery-v1-tests` adds authority for:

- `<>` and a non-fixture `<1x` invalid-ASCII tag-open example;
- `</>` empty-end-tag recovery;
- all five unquoted attribute-value special bytes, including exact error position and retained payload;
- explicit proof that generic tag-open recovery does not accidentally admit NUL or non-ASCII preprocessing debt.

The dedicated `html-tokenizer-numeric-character-reference-v1-tests` retains authority for literal ampersand fallback, decimal/hex numeric decoding, exact numeric recovery errors, C1/noncharacter/scalar validation, decoded UTF-8 output, Data coalescing, quoted/unquoted attribute integration and replacement byte caps. `html-tokenizer-named-character-reference-v1-tests` adds full pinned-table longest-match, two-scalar output, legacy-semicolon recovery, attribute veto, ambiguous-ampersand and named replacement-cap authority.

The pinned html5lib `test1.test` runner denominator remains separate authority. Production capability becoming broader does not itself promote historical unsupported cases into passes.

## Claim boundary

This slice does not satisfy `html_tokenizer_conformance`. It expands the production token surface required to run more of the pinned external corpus honestly.

The admitted `test1.test` runner remains a separate authority surface, and unsupported cases continue to be counted explicitly rather than converted to passes. Raw-input preprocessing/non-ASCII authority, broader recovery, CDATA and tree-builder conformance remain open.

`tree_builder_conformance` remains independently outstanding and Z7 remains `planned`.
