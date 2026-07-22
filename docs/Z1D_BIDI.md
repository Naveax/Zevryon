# Z1D — Bounded Unicode 17 Bidirectional Resolution

Z1D adds paragraph-level Unicode Bidirectional Algorithm processing on top of the bounded UTF-8, grapheme and script-run substrates.

## Standards and data identity

- Unicode version: `17.0.0`
- UAX #9 revision: 51
- generated data fingerprint: `db4c3660e8e846eb5805bcdbedca8c96f84967ad136b3305b6bf0d33409a8e91`
- generated header size: 94,929 bytes

Pinned source hashes:

- `DerivedBidiClass.txt`: `4867b4b7f0731ed1bfcd34cc6251211ff1542541fce0734b6fbda139ee80b3a4`
- `BidiBrackets.txt`: `dadbaf38a0d0246e5b805bf8725cb81b7c621f93d030595635f5ba2c2f179428`
- `BidiMirroring.txt`: `a2f16fb873ab4fcdf3221cb1a8a85a134ddd6ed03603181823ff5206af3741ce`
- `PropertyValueAliases.txt`: `64e9a5f76f7a1e8b5a47d6a1f9a26522a251208f5276bdfa1559dac7cf2e827a`
- `BidiTest.txt`: `888bdfc8090652272d1f859cdb00ae659e2dc6c26740be61ef1d03998a687620`
- `BidiCharacterTest.txt`: `a3e6e905ab5afbe318a96df5401d0372a04cd73ef139ab5e3cf0ae241c255488`

CI downloads the versioned files, verifies every hash, regenerates the C++ tables and compares the result byte-for-byte with the vendored header and manifest.

Generated property tables contain:

- 23 `Bidi_Class` values;
- 1,267 compact class ranges;
- 128 paired-bracket entries;
- 428 mirroring entries.

## Property API

The runtime exposes zero-allocation binary-search lookup for:

- `Bidi_Class`;
- paired-bracket codepoint and bracket type;
- mirrored codepoint;
- short and long property aliases.

No runtime Unicode parser, hash map or hidden per-codepoint heap allocation is used.

## Resolver pipeline

The bounded resolver implements the paragraph-level UAX #9 rule sequence:

1. P2/P3 paragraph direction;
2. X1–X10 explicit embeddings, overrides and isolates;
3. X9 removal of formatting codes;
4. W1–W7 weak-type resolution;
5. N0 paired-bracket processing;
6. N1/N2 neutral resolution;
7. I1/I2 implicit levels;
8. L1 whitespace and segment reset;
9. L2 visual ordering.

The resolver supports:

- explicit LTR, explicit RTL and automatic paragraph direction;
- LRE/RLE/LRO/RLO/PDF;
- LRI/RLI/FSI/PDI and isolating run sequences;
- bounded explicit-level stacks;
- bounded bracket stacks;
- paired-bracket canonical-equivalence exception for U+232A and U+3009;
- output levels for every logical input unit;
- `0xFF` sentinel levels for X9-removed units;
- visual order containing only non-X9-removed logical indices.

Synthetic `Bidi_Class` input is supported for `BidiTest.txt`. Real decoded scalar input uses the Unicode property and paired-bracket tables for `BidiCharacterTest.txt` and browser text.

## Memory contract

All persistent output and temporary resolver allocations use the Z0 `BidiBuffer` Resource Ledger class.

The ledger reserves bytes before allocation and reports:

- current and peak bytes;
- rejected reservations;
- accounting errors.

Budget exhaustion returns a controlled `OutputBudgetExceeded` error. No partial level or visual-order output is retained after failure.

The final certification limit for the 64 KiB mixed-direction workload is 2 MiB. The observed peak before final tightening was approximately 1.25 MiB; a 1 MiB claim would therefore be false.

## Conformance gates

The final CI contract requires:

- `BidiTest.txt`: 490,846 source rows and 770,241 paragraph-mode resolutions;
- `BidiCharacterTest.txt`: 91,707 real-codepoint tests and 717,503 total codepoints;
- maximum synthetic corpus input length: 76 units;
- maximum real-codepoint corpus input length: 130 units;
- all expected paragraph levels, resolved levels and L2 orders to match;
- exact regeneration of the property tables;
- adversarial unit tests;
- Ubuntu, macOS and Windows strict C++20 builds;
- Linux ASan/UBSan;
- dedicated Windows MSVC AddressSanitizer.

## Performance gate

The benchmark prepares a 64 KiB mixed-direction scalar span before timing. The measured section includes:

- Unicode bidi-property lookup;
- paragraph-level selection;
- explicit and isolate processing;
- weak, bracket and neutral rules;
- implicit levels and L1 resets;
- L2 visual-order materialization.

Three independent distributions are used, each with 1,024 measured iterations after 32 warmups. The final gates are:

- median P95 no greater than 2.25 ms;
- median P99 no greater than 2.75 ms;
- worst observed maximum no greater than 4.00 ms;
- peak BidiBuffer allocation no greater than 2 MiB;
- zero rejected reservations;
- zero accounting errors.

## Explicit boundary

Z1 remains active after Z1D. The resolver is paragraph-level UAX #9 infrastructure; it does not generate Unicode line-break opportunities or perform font shaping.

Remaining work before Z1 can be marked implemented:

- Z1E Unicode line breaking;
- integration of grapheme, script, bidi and line-break boundaries into Z2 shaping.
