# Z7 HTML tokenizer Data UTF-8 text v1

## Purpose

This production slice admits well-formed UTF-8 Unicode scalar sequences as ordinary character data in the existing bounded Data tokenizer. It closes the raw-Data-text gap needed for literal non-ASCII characters while keeping broader HTML input preprocessing and non-ASCII markup surfaces fail-closed.

## Admitted behavior

- ASCII Data behavior remains unchanged.
- A non-ASCII Data byte must begin one canonical well-formed UTF-8 scalar sequence.
- 2-, 3- and 4-byte scalar encodings are validated for continuation structure, overlong encodings, surrogate exclusion and the U+10FFFF upper bound.
- Valid scalar bytes are preserved byte-for-byte in the coalesced Character token.
- Ordinary Data-state U+0000 emits `unexpected-null-character` and is preserved as literal U+0000 Character data.
- The existing `maximum_token_bytes` bound applies to the complete UTF-8 byte sequence before publication.
- Character-reference fallback followed by a non-name Unicode scalar remains literal Data, including the pinned html5lib `&¬;` shape.
- Parse-error columns in the Data tokenizer and character-reference diagnostics count admitted UTF-8 scalars instead of encoding bytes.
- The Data-stream coordinator advances delegated source columns by admitted UTF-8 scalars so later segment diagnostics keep the same coordinate basis.

## Fail-closed boundary

This slice still rejects:

- malformed, truncated, overlong, surrogate or out-of-range UTF-8;
- U+0000 handling after transitions into tag, attribute, comment or DOCTYPE states;
- CR/LF input-stream normalization;
- non-ASCII tag names;
- non-ASCII attribute names;
- raw non-ASCII attribute values;
- any broader tokenizer state or preprocessing behavior not already admitted.

The scalar validator is intentionally an encoding boundary, not a substitute for the WHATWG input-stream preprocessing algorithm.

## Verification

The dedicated `html-tokenizer-data-utf8-text-v1-tests` target freezes valid 2/3/4-byte passthrough, literal ampersand fallback, scalar-aware error columns, malformed UTF-8 rejection, token-byte bounds, ordinary Data-state U+0000 authority, and the retained non-ASCII markup guards.

The production probe is additionally checked with the exact UTF-8 bytes for `&¬;` so this slice can later support a separate external-authority promotion without coupling production behavior to the authority runner.

## Nonclaims

This slice does not by itself claim complete html5lib `test1.test`, complete input-stream preprocessing, arbitrary non-ASCII markup support, full WHATWG tokenizer conformance, `html_tokenizer_conformance`, tree-builder conformance, or Z7 completion.
