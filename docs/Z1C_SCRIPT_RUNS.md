# Z1C — Bounded Unicode Script Runs

Z1C adds shaping-oriented Unicode `Script` and `Script_Extensions` resolution on top of the bounded UTF-8 and grapheme substrates.

## Data contract

- Unicode version: `17.0.0`
- `Scripts.txt` SHA-256: `9f5e50d3abaee7d6ce09480f325c706f485ae3240912527e651954d2d6b035bf`
- `ScriptExtensions.txt` SHA-256: `ec2107e58825a1586acee8e0911ce18260394ac8b87e535ca325f1ccbeb06bc6`
- `PropertyValueAliases.txt` SHA-256: `64e9a5f76f7a1e8b5a47d6a1f9a26522a251208f5276bdfa1559dac7cf2e827a`
- generated data fingerprint: `8f402bcab66ac57762f8078956a562f3e702aed37c5d32f290821dceeb573c18`

The generator is deterministic. CI downloads the versioned sources, verifies all three hashes, regenerates the header and manifest twice, and compares the generated output byte-for-byte with the vendored files.

Generated table dimensions:

- 176 script identifiers;
- 984 merged primary-script ranges;
- 176 merged Script_Extensions ranges;
- 118 unique extension sets;
- 536 ScriptId entries in the shared extension pool;
- 82,922-byte generated header.

## Runtime lookup

`script_of(codepoint)` performs a binary search over the non-overlapping primary-script ranges. Missing codepoints resolve to `Unknown` (`Zzzz`).

`script_extensions(codepoint)` performs a binary search over explicit Script_Extensions ranges. When no explicit range exists, it returns a zero-allocation singleton view containing the primary Script value.

No hash map, runtime Unicode parser or hidden heap allocation is used by property lookup.

## Run resolution

The resolver consumes:

1. a contiguous decoded scalar span;
2. the grapheme cluster start boundaries plus final sentinel produced by Z1B.

A grapheme cluster is never split. For each cluster, strong Script_Extensions sets are intersected. Common, Inherited and Unknown values are neutral unless an explicit Script_Extensions set supplies strong candidates.

Across clusters, candidate sets are intersected until they become disjoint. A disjoint strong cluster starts a new run. Leading neutral clusters attach to the first following strong run; middle and trailing neutral clusters remain with the preceding run. An all-neutral span resolves to Common (`Zyyy`).

If incompatible strong scripts occur inside one grapheme cluster, grapheme atomicity wins: the first strong scalar candidate remains authoritative and the conflict is counted.

This is a deterministic shaping-oriented partition. It does **not** perform bidi embedding, isolate processing, visual reordering or line breaking.

## Memory contract

Output uses 16-byte `ScriptRunBoundary` records:

- absolute source byte offset;
- cluster index;
- resolved ScriptId;
- reserved field.

Non-empty output contains one boundary for every run start plus one final sentinel. Source and cluster lengths are derived from adjacent boundaries.

All output allocation is charged to the Z0 `ScriptRun` Resource Ledger class before allocation. Budget exhaustion is a controlled `OutputBudgetExceeded` result.

## Correctness gates

CI verifies:

- all 1,114,112 Unicode codepoints against `Scripts.txt`;
- all 206 source Script_Extensions ranges;
- default singleton Script_Extensions behavior outside explicit ranges;
- 2,287 source Script ranges;
- 669 codepoints with explicit Script_Extensions;
- representative Latin, Greek, Cyrillic, Arabic, Han, Hiragana, Katakana, Devanagari, Common, Inherited and Unknown behavior;
- leading/trailing neutrals, combining marks, extension-set intersections, invalid sentinels and hard-budget rejection;
- Ubuntu, macOS and Windows strict builds;
- Linux ASan/UBSan and dedicated Windows MSVC ASan.

## Performance certification

The certification fixture represents 64 KiB of mixed-script source. UTF-8 decoding and grapheme segmentation are performed before timing. Only Script lookup, Script_Extensions intersection and run-boundary materialization are measured.

The final gate uses three independent 1,024-iteration distributions after 32 warmups:

- median P95 at most 1.50 ms;
- median P99 at most 2.00 ms;
- worst observed maximum at most 3.00 ms;
- ScriptRun peak allocation at most 256 KiB;
- zero rejected reservations and zero accounting errors.

## Remaining Z1 work

Z1 remains active. The remaining required layers are:

- Unicode bidi resolution and visual run ordering;
- Unicode line-break opportunity generation;
- integration of script, bidi and line-break runs into Z2 shaping.
