# Z7 HTML tokenizer input newline preprocessing v1

This slice admits the HTML input-stream newline normalization required by the pinned html5lib tokenizer corpus at the canonical `tokenize_html_token_stream_v1()` boundary.

## Production contract

Before tokenizer-state dispatch, the bounded raw input is normalized as follows:

- `CRLF` becomes one `LF`;
- an isolated `CR` becomes one `LF`;
- input without `CR` remains a non-owning view and performs no normalization allocation;
- when normalization is required, retained temporary storage never exceeds the already-validated raw input byte length;
- `HtmlTokenizerV1Stats::input_bytes` continues to report the original raw input byte count rather than the shorter normalized view.

The normalized view is shared by Data, PLAINTEXT, RCDATA, RAWTEXT, Script-data and CDATA initial-state dispatch. This keeps markup/comment parsing and text states on one input-preprocessing authority instead of duplicating newline rules inside individual states.

## Corpus evidence

Diagnostic run `34598730563` applied the production patch in an isolated CI workspace and measured the complete pinned 7036-execution corpus before publication:

- v8 authority: **6743 pass / 122 fail / 171 unsupported**;
- newline candidate: **6775 pass / 122 fail / 139 unsupported**;
- movement: **+32 pass / 0 fail / -32 unsupported**;
- `unsupported:input-preprocessing-cr`: **32 -> 0**.

Only three fixture reports moved: `domjs.test` gained 3 exact passes, `test3.test` gained 21, and `test4.test` gained 8. All existing failure buckets remained unchanged.

## Nonclaims

This slice does not implement NUL replacement, malformed/non-scalar input preprocessing, remaining non-ASCII tokenizer debt, XML infoset coercion, tree construction, or full tokenizer conformance. The generic NUL preprocessing debt remains separately fail-closed and the pinned CDATA raw-NUL behavior remains unchanged.
