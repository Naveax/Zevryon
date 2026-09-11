# Z7 bounded CDATA section initial state v1

This slice admits the pinned html5lib `CDATA section state` initial-state surface at the canonical token-stream boundary.

## Admitted behavior

- bytes before the first `]]>` are emitted as Character data;
- an extra closing bracket before `]]>` remains Character data;
- `]]>` itself emits no token and transitions to the already-admitted canonical Data stream;
- adjacent Character data is coalesced across the CDATA-to-Data transition;
- EOF before `]]>` emits `eof-in-cdata` at the exact EOF location while preserving accumulated Character data;
- admitted ASCII input controls retain `control-character-in-input-stream` diagnostics;
- raw NUL in this specific CDATA authority remains a literal NUL Character, matching the pinned html5lib fixture rather than the generic Data/text-state NUL debt bucket;
- Character output remains bounded by `maximum_token_bytes`, including coalescing across the Data transition.

## Nonclaims

This does not admit general raw-NUL replacement, CR normalization, malformed UTF-8/non-scalar input, XML infoset coercion, or complete tokenizer conformance.
