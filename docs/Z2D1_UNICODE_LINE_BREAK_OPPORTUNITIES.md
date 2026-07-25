# Z2D-1 Unicode Line-Break Opportunities

Z2D-1 adds the bounded opportunity-discovery stage between grapheme segmentation and width-constrained line selection.

## Contract

The output contains exactly one byte for each logical grapheme boundary, including the terminal boundary. Values are `Prohibited`, `Allowed`, or `Mandatory`. Z2D-1 does not choose where a line actually ends; Z2D-2 will combine these opportunities with shaped advances and available inline width.

## Unicode model

The rule engine implements Unicode 17.0.0 UAX #14 revision 55, including the Unicode 17 `HH` class and Brahmic rules. Mandatory break characters are handled before the documented grapheme-cluster tailoring. Each remaining grapheme cluster is represented by its first code point, while LB9/LB10 combining behavior remains explicit for defensive or conformance inputs that expose marks as separate clusters.

Generated lookup data covers all Unicode scalar values. The normalized line-break class and contextual-flag fingerprint is:

`3f256b5af69b5e7cfd5e9dddfbbe3b28cb18c98746dca47753b102fa3e99a4e4`

## Memory and failure behavior

Retained output is exactly `cluster_count + 1` bytes. Construction uses one temporary packed 64-bit record per significant cluster. The caller controls both retained and temporary storage through the PMR resource and may charge that resource to the appended `LineBreakOpportunityMap` Resource Ledger class.

The builder clears prior output before validation and publishes the replacement only after complete success. It rejects invalid scalar/source streams, incomplete or overlapping grapheme topology, hidden mandatory breaks, compact-domain overflow, allocation rejection, and aggregate counter overflow.

## Complexity

Construction is `O(codepoints + clusters + significant boundaries)` with binary-search Unicode property lookup. Boundary lookup is `O(1)` and allocation-free.

## Certification

The focused certification includes:

- the official Unicode 17 `LineBreakTest.txt` default-rule corpus using singleton logical clusters;
- grapheme-tailored production fixtures, including CRLF, combining marks and emoji sequences;
- strict GCC, AppleClang and MSVC diagnostics;
- Linux ASan/UBSan and Windows AddressSanitizer;
- a 64 KiB mixed-script workload with an exact one-byte-per-boundary retained result and a 512 KiB total PMR hard cap.

The singleton conformance mode certifies the default UAX #14 rule core. The production API additionally applies the disclosed grapheme-cluster tailoring.

## Explicit boundary

This stage does not implement CSS `line-break`, `word-break`, `overflow-wrap`, dictionary segmentation, language-specific hyphenation, shaped-width fitting, visual bidi line ordering, fragments, hit testing, rasterization, or painting. Those remain subsequent Z2D/Z2E/Z2F partitions.
