# Z7 html5lib tokenizer runner v1

## Purpose

The frozen `contentModelFlags.test` corpus is external authority only if the expected token/error streams are read from the vendored upstream JSON and compared with events emitted by the production tokenizer boundary. Re-typing the same expectations into a C++ unit test is useful regression coverage, but it is not an external corpus run.

This slice adds that missing execution layer without broadening the conformance claim beyond the one pinned fixture.

## Architecture

The runner is deliberately split into two parts:

1. `zevryon-html-tokenizer-token-stream-v1-probe` is a C++ executable linked against `zevryon-massivedoc-core`. It calls `tokenize_html_token_stream_v1()` directly and serializes the resulting token events, parse-error events and statistics through a deterministic line protocol.
2. `scripts/z7_html5lib_tokenizer_runner_v1.py` verifies the frozen corpus provenance, reads the vendored upstream JSON, expands every declared initial state, invokes the C++ probe and compares the actual production event stream with the external expected stream.

Python is used only as the corpus-format adapter. Tokenization semantics remain in production C++ code. The probe does not contain a duplicate tokenizer implementation and the runner does not synthesize tokens from node-source output.

## Authority binding

V1 PASS authority is deliberately tied to the canonical repository paths:

- `config/z7_html5lib_tokenizer_corpus.json`;
- `tests/fixtures/html5lib-tokenizer/contentModelFlags.test`.

Alternate manifest or fixture paths are rejected before execution. This prevents a caller from verifying the canonical provenance manifest but substituting a different 14-test JSON file and obtaining a misleading `fixture_pass_claim`.

The canonical manifest verifier still enforces the exact upstream repository, commit, Git blob, SHA-256, byte counts and 14-test/24-execution cardinality before the runner reads expected outputs.

## Probe protocol

Probe arguments are:

`<PLAINTEXT|RCDATA|RAWTEXT> <last-start-tag-hex> <input-hex>`

Strings are transported as hexadecimal bytes so tabs, newlines and other payload bytes cannot become protocol delimiters.

Successful output is composed of deterministic records:

- `TOKEN\tC\t<hex>` for Character tokens;
- `TOKEN\tE\t<hex>` for EndTag tokens;
- `ERROR\t<code-hex>\t<line>\t<column>` for tokenizer parse errors;
- exactly one `STATS` record containing input bytes, total tokens, Character-token count, Character bytes, EndTag count and parse-error count.

An unsupported/fail-closed tokenizer execution emits a `FAIL` record and exits nonzero. Bad probe arguments use a distinct usage exit code. Successful probe executions must not emit stderr; unexpected protocol records, duplicate/missing stats, invalid hex, invalid positions or negative counters are rejected by the adapter.

## Frozen fixture execution

Before any tokenizer execution, the runner calls the admitted html5lib provenance verifier. The corpus must still resolve to the exact frozen authority:

- 14 test objects;
- 24 executions after expanding `initialStates`;
- byte-identical fixture/license provenance already pinned by the corpus manifest and verifier.

For each execution the runner compares, in order:

- token type;
- token payload/name bytes;
- parse-error code;
- parse-error line and column;
- input byte count;
- emitted token counts and Character-byte accounting.

Unknown initial states, token types outside the admitted v1 event surface, `doubleEscaped` cases or input outside the currently admitted ASCII location/preprocessing authority are counted as `unsupported`; they are never silently skipped. Any failed or unsupported execution makes the CTest fail.

For the current frozen `contentModelFlags.test` fixture, the intended admitted result is therefore exactly:

- 24 passed;
- 0 failed;
- 0 unsupported.

That result is a **pinned-fixture pass claim**, not a full tokenizer-conformance claim.

## CTest integration

`z7-html5lib-tokenizer-runner-v1` is registered when a Python 3 interpreter is available. CMake passes the exact built probe path with `$<TARGET_FILE:...>`, so multi-configuration Windows builds and single-configuration Linux builds both execute the binary produced by the same source tree under test.

No network access is required. CI consumes only the vendored, provenance-verified corpus.

## Claim boundary

The machine report intentionally separates:

- `fixture_pass_claim`: true only when every one of the 24 frozen executions passes with zero unsupported cases;
- `html_tokenizer_conformance_claim`: always false in v1;
- `z7_status_change`: false.

Passing this fixture proves the production token-event boundary agrees with this exact external html5lib fixture. It does not establish coverage of the rest of html5lib tokenizer tests and therefore does not satisfy the canonical `html_tokenizer_conformance` gate.

Still outstanding include general Data-state tokenization, start tags and attributes, comments, DOCTYPE, the complete character-reference algorithm, script-data, CDATA, input preprocessing/NUL replacement, broader parse-error recovery and expansion to the complete frozen tokenizer corpus with explicit pass/fail/unsupported accounting.

The tree builder is independently governed by the WPT corpus authority. Z7 remains `planned`.
