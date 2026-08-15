# M4 — Bounded Search and Full-Document Operations Promotion

This receipt promotes the M4 bounded-search and full-document-operations authority only after the implementation source head and the evidence-only publication head are independently exact-head green.

## Frozen source authority

- repository: `Naveax/Zevryon`
- branch: `agent/m4-bounded-search`
- base `main`: `4101680d1cb07af67fe280de04187a275e68124a`
- source authority head: `6b3124c28af9b4da84badd88a1be628971df6a0e`
- source relation to main: 19 ahead / 0 behind
- merge base: exact `4101680d1cb07af67fe280de04187a275e68124a`
- changed implementation/test/build/workflow/evidence paths: 55
- exact source CI: `Windows and Linux CI` run `31881129599`, 5/5 SUCCESS
- dedicated Unicode 17 authority: run `31881129602`, SUCCESS

The source authority above is the exact runtime/test/build/workflow state being certified. This promotion commit is intentionally evidence-only and must not alter implementation, tests, build wiring, workflow behavior, generated Unicode data, or runtime behavior.

## Admitted M4 authority

The admitted line provides:

1. bounded block Bloom summaries and compressed on-disk trigram postings for exact byte-search candidate acceleration;
2. source-identity binding between derived search data and the canonical immutable payload authority;
3. canonical trigram-store generation and opportunistic `StoreReader::find_bounded()` acceleration;
4. authoritative exact verification with fail-closed fallback when derived search data is missing, stale, corrupt, incompatible, or unreadable;
5. bounded cancellation that never promotes partial accelerated results to authority after cancellation;
6. Unicode 17.0.0 search normalization using `Q(X)=NFC(toCasefold(NFD(X)))`, with bounded pending state and source-byte span propagation;
7. strict normalized UTF-8 search with explicit invalid-input and normalization-bound failures rather than silent semantic fallback;
8. an O(1) full-document selection descriptor backed by an immutable logical-order snapshot and root aggregate metadata;
9. fixed-memory logical-order text and escaped HTML export with transactional target preservation and cancellation;
10. a cross-architecture exact-byte matcher with scalar fallback, x86-64 SSE2 runtime coverage on Linux and Windows, and randomized scalar-equivalence coverage;
11. production `StoreReader` exact verification routed through that matcher, with runtime counters proving SIMD batches on SIMD-capable x64 builds.

## Search acceleration and exactness

Trigram/Bloom structures are derived acceleration data, not source authority. Exact payload verification remains authoritative.

The admitted search path therefore preserves these rules:

- candidate data must match canonical source identity;
- derived-data failure cannot create a false negative by becoming authoritative;
- missing/corrupt/stale derived data falls back to the legacy exact path;
- cancellation aborts without returning partial accelerated hits as authoritative results;
- exact matching retains first-match semantics and scalar equivalence.

The final source head removes the remaining production `std::search` verifier call-site from `StoreReader::find_bounded()` and routes it through `find_exact_bytes()`. `SearchExecutionStats` aggregates SIMD batches, scalar candidates, and exact compares. Test #43 contains a real `StoreWriter`/`StoreReader` fixture and requires a nonzero SIMD batch count when the build reports SIMD availability, so the Linux and Windows x64 PASS results are production-call-site evidence rather than source-presence evidence.

## Unicode 17 authority

The Unicode normalization data remains locked to the admitted Unicode 17.0.0 authority:

- transform: `Q(X)=NFC(toCasefold(NFD(X)))`;
- official suite: `NormalizationTest-17.0.0`;
- official conformance: `20,034 / 20,034 PASS`;
- generated table SHA-256: `40730ad16b9adddbf71f59ec54c8089a3a129751192bca7c47a6a527a68b78ff`;
- generated fingerprint: `4194e1873cf16402211f44a847617746ad975a599c3a206e88c1a9f43cf3b70c`;
- exact source-head dedicated authority run: `31881129602`, SUCCESS;
- authority artifact: `9246037267`, digest `sha256:b41ed7bdc442286a0744c1841730e4bc6366e91eb9511d59ab6f5715038eb241`;
- generated-data drift at the frozen source head: none.

Normalized search deliberately does not use the raw-byte trigram index as a mandatory candidate eliminator, because raw-byte trigrams can create false negatives across canonical equivalence or full case-fold expansion. The admitted Unicode path prioritizes exact normalized semantics and bounded execution over an unsound raw-byte shortcut.

## O(1) full-document selection

Full-document selection is represented by an immutable logical-order snapshot plus root aggregate metadata. Constructing the select-all descriptor does not traverse records or text; the root aggregate supplies record count and total logical text bytes directly.

The descriptor therefore remains stable across later mutable-order operations on the live sequence and represents the logical text half-open range `[0, total_text_bytes)` for its captured snapshot.

## Fixed-memory export

Text and HTML export walk the immutable logical-order snapshot and stream source records without whole-document buffering.

The admitted export path provides:

- logical-order text streaming;
- escaped HTML source-text streaming inside the bounded HTML contract;
- strict UTF-8 validation for HTML;
- fixed-size input/cancellation/output working buffers;
- metadata/source-length consistency checks;
- transactional temporary-file publication;
- preservation of the previous target on cancellation or failure.

This is not a semantic DOM reconstruction claim. The admitted HTML surface is bounded escaped source-text export.

## Exact source-head CI

`Windows and Linux CI` run `31881129599` completed 5/5 SUCCESS on `6b3124c28af9b4da84badd88a1be628971df6a0e`:

- Linux build + headless `95003739592`: SUCCESS, 96/96 CTest;
- Windows build + headless `95003739595`: SUCCESS, 60/60 CTest;
- Linux Unicode authority `95003739561`: SUCCESS;
- Windows Unicode authority `95003739586`: SUCCESS;
- Apple backend removal guard `95003739656`: SUCCESS.

On both Linux and Windows, Test #43 `massivedoc-exact-match-tests` passed with the production StoreReader integration fixture.

The Linux job also re-passed both inherited M3 storage regression gates on the exact M4 source head:

- exact 4 GiB legacy-profile canonical-generation open: PASS;
- bounded cold-store PSS gate: PASS.

Exact source-head artifacts:

- Linux Unicode: artifact `9246058280`, digest `sha256:89e298437ebe253527179676d703041c0d0a2df1586de8fb4a2045c5b68c08fe`;
- Windows Unicode: artifact `9246124345`, digest `sha256:1579bdafbe000c6a3ff070015569a54d66cf7ce204bbcb9d4a8b12d283fb3474`;
- inherited M3 storage regression evidence: artifact `9246069702`, digest `sha256:720f87ce5c18b33a0c273e91918b82d3fe794af7f7f5e7a0bc2fc54c67bd6c74`.

Dedicated Unicode 17 authority run `31881129602` also completed SUCCESS on the exact source head with no generated-data drift.

## Deliberate boundaries

This receipt does not claim:

- ARM64 NEON runtime certification. The NEON implementation exists in source, but this CI matrix has no ARM64 runner;
- locale-specific collation or locale-specific casing beyond Unicode default full case-fold;
- normalized-query acceleration from the raw-byte trigram index;
- semantic DOM reconstruction in HTML export;
- unbounded whole-document buffering or unbounded normalized-search state.

The scalar exact matcher remains the fallback authority when a supported SIMD implementation is unavailable.

## Evidence-head admission rule

This promotion commit adds only:

- `certification/m4_bounded_search_promotion.json`
- `docs/M4_BOUNDED_SEARCH_PROMOTION.md`

Because those files are evidence-only, the frozen source authority remains `6b3124c28af9b4da84badd88a1be628971df6a0e`.

The branch is not fully promoted merely because this receipt exists. The exact evidence-only child head must itself receive a successful push-triggered `Windows and Linux CI` run. After that, the promotion PR may be opened and merged only against the expected canonical base.

Merge credit is still not granted by the evidence-head CI alone. The exact merged `main` head must receive its own successful push-triggered post-merge CI run before M4 is considered canonically closed and Issue #100 may be closed.
