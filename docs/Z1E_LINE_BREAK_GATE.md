# Z1E Unicode line-break conformance gate

Z1 requires `line_break_conformance`. The production Unicode 17 line-break engine already exists as the bounded `LineBreakOpportunityMap` authority introduced by Z2D-1; this gate reuses that implementation rather than creating a second Unicode line-breaking stack.

## Authority

- Unicode version: `17.0.0`.
- UAX #14 revision: `55`.
- Normative corpus: `LineBreakTest-17.0.0.txt`.
- Normative corpus SHA-256: `e69884e0dde6a8724873f885d68c52dc14518abf9ae4ca9e2283b8773db3b752`.
- Production executable: `zevryon-line-break-conformance`.
- Production focused test: `line-break-opportunity-tests`.

The conformance executable constructs singleton logical clusters from the normative corpus, runs the real production `build_line_break_opportunity_map` entry point, and compares every expected boundary. The production API retains its documented grapheme-cluster tailoring for browser-facing use.

## Required result

The frozen Unicode 17 corpus contains 19,338 normative cases, 60,487 code points, and 79,825 inspected boundaries. Certification requires zero parse failures, zero build failures, and zero correctness failures on both Linux and Windows.

The historical Z2D-1 certification reached 19,338 / 19,338 passing cases. Z1 credit is granted only by the exact-head workflow in `.github/workflows/z1-line-break-conformance.yml`; historical evidence alone is not sufficient to change the milestone state.

## Boundary

This gate certifies Unicode default line-break opportunity discovery. CSS `line-break`, `word-break`, `overflow-wrap`, dictionary segmentation, language-specific hyphenation, width fitting, and visual line layout are separate higher-layer responsibilities and are not part of the Z1 Unicode substrate gate.
