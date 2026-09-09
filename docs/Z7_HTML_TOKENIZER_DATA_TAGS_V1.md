# Z7 bounded HTML Data-tag tokenizer v1

## Purpose

The first frozen html5lib tokenizer fixture exercises explicit PLAINTEXT, RCDATA and RAWTEXT initial states, but the broader pinned corpus defaults missing `initialStates` to Data state. A genuine expansion of `html_tokenizer_conformance` therefore needs a production Data-state token boundary rather than a runner-side approximation.

This slice adds a bounded Data-state tag tokenizer that emits the existing `HtmlTokenizerV1Token` event representation through the existing `HtmlTokenizerV1Sink`. It is intentionally separate from comments, DOCTYPE, character references and script-data so each authority can be admitted and measured independently.

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

The event boundary is shared with the existing tokenizer stream. StartTag tokens therefore already carry ordered `HtmlTokenizerV1Attribute` records and the self-closing flag rather than being reconstructed from node-source output.

## Parse-error authority

The slice admits two external-corpus-relevant recovery diagnostics while still completing tokenization:

- `missing-whitespace-between-attributes` when a completed attribute is followed immediately by the next admitted attribute;
- `duplicate-attribute`, with the first attribute value retained and the duplicate dropped.

End-tag attributes are parsed for bounded validation, then omitted from the emitted EndTag token and reported as `end-tag-with-attributes`. A self-closing end tag is likewise diagnosed as `end-tag-with-trailing-solidus` and emitted as an ordinary EndTag.

Parse-error line/column positions are one-based. V1 location authority is deliberately ASCII-only, so byte offsets and character columns are identical. Non-ASCII input remains fail-closed until preprocessing and Unicode location accounting are admitted.

## Bounds

Default caller bounds are:

- input: 1 MiB;
- token payload: 64 KiB;
- emitted attributes per tag: 256.

Fixed implementation maxima are 16 MiB input, 1 MiB token payload and 4096 attributes. Character buffering, names, values and aggregate emitted tag payload are all bounded before publication.

The sink is streaming. A later unsupported construct can therefore fail after earlier complete token events have already been delivered. Within a token, however, the token is not published until that token has passed its admitted validation and bounds.

## Deliberate fail-closed surface

The following remain outside this slice:

- `<!...` markup declarations, comments and DOCTYPE;
- `<?...` bogus-comment recovery;
- named or numeric character references in Data or attribute values;
- NUL replacement and complete input-stream preprocessing;
- non-ASCII preprocessing/location authority;
- malformed tag-open and malformed attribute recovery outside the explicitly admitted diagnostics;
- script-data and CDATA states.

These cases return an explicit API failure instead of fabricating a token stream.

## Tree-builder feedback boundary

`<plaintext>` in Data state is emitted only as a StartTag token. This tokenizer slice does **not** switch itself to PLAINTEXT after emitting that token. In the HTML parsing algorithm, tokenizer state changes of that kind are driven by the tree-builder consumer. Preserving that separation is necessary for a real tokenizer/tree-builder boundary.

## Focused regression authority

The C++ regression suite covers:

- normalized start/end tags and Character coalescing;
- quoted, unquoted and empty attributes;
- self-closing start tags;
- exact `missing-whitespace-between-attributes` location matching the pinned html5lib `test1.test` example;
- exact duplicate-attribute first-wins behavior/location;
- end-tag attribute diagnostics and attribute suppression;
- `<plaintext>` StartTag emission without fake tree-builder feedback;
- explicit fail-closed DOCTYPE, character-reference and NUL boundaries;
- token and attribute hard caps.

This is implementation authority only. The pinned `test1.test` file is not yet vendored/executed by this slice, and the complete html5lib corpus is much broader.

## Claim boundary

This slice does not satisfy `html_tokenizer_conformance`. It expands the production token surface required to run more of the pinned external corpus honestly.

The external `contentModelFlags.test` runner remains the first frozen fixture execution authority. Broader corpus admission still requires Data-state character references, comments, DOCTYPE, preprocessing/doubleEscaped handling, script-data, CDATA and explicit whole-corpus pass/fail/unsupported accounting.

`tree_builder_conformance` remains independently outstanding and Z7 remains `planned`.
