# Z2E-2 — Bounded Line-Box Metrics

## Purpose

Z2E-2 converts Z2E-1 visual fragment slices into horizontal line boxes with deterministic font metrics, baseline alignment and cumulative block-axis positions. It is the first stage that gives retained shaped text a physical block extent.

## Font metric extraction

`FontLineMetricRecord` is exactly 32 bytes and stores copied scalar metrics only. It does not retain font bytes.

The extractor consumes a structurally verified `SfntResourceView` and requires a valid `head.unitsPerEm` value. Metric source selection is deterministic:

1. use OS/2 `sTypoAscender`, `sTypoDescender` and `sTypoLineGap` when `fsSelection.USE_TYPO_METRICS` is set;
2. otherwise use a valid `hhea` ascender, descender and line gap;
3. otherwise use a valid OS/2 typographic triple as an explicit fallback.

An explicitly selected but invalid OS/2 typographic triple fails closed. Negative line gaps are preserved rather than silently clamped.

`FontLineMetricTable` stores exactly one record per immutable catalog binding, sorted strictly by `face_id`. Every source binding must belong to one catalog generation.

## Line-box output

`LineBoxRecord` is exactly 48 bytes and retains:

- cumulative block start;
- line block size;
- absolute baseline position;
- inline advance inherited from Z2E-1;
- one contiguous fragment-metric slice;
- logical cluster limit and inherited line flags.

`FragmentBlockMetric` is exactly 32 bytes and is parallel to the Z2E-1 visual fragment array. It retains block offset inside the line, fragment block size, baseline offset and compact metric-source flags.

## Scaling and leading

Design-space ascender, descender and line gap values are scaled with the exact positive `y_scale` retained by HarfBuzz. Integer conversion uses deterministic nearest rounding.

The signed scaled line gap is split into block-before and block-after leading. Division truncates toward zero and the odd signed remainder is assigned to block-after, so both parts always sum exactly to the original scaled gap.

Each line begins with a strut metric. Fragment metrics can expand the line ascent or descent but cannot shrink the strut. Empty lines therefore receive a stable block size and baseline.

## Baseline alignment

For every fragment:

`fragment.block_offset = line_ascent - fragment.baseline_offset`

The line baseline is:

`line.block_start + line_ascent`

Lines remain in logical block order. Their block starts form one checked 64-bit cumulative partition.

## Failure atomicity

The caller-visible vectors are released before validation and remain empty after every failure. Metric-table validation, topology checks, scaling overflow, missing face records, PMR budget exhaustion and aggregate position overflow all fail closed.

The line-box builder allocates only its two retained exact-size output vectors. It does not copy glyphs or create another full fragment array.

## Certification corpus

The fixed benchmark uses:

- 65,536 logical clusters represented by Z2E-1 ranges;
- 1,024 lines;
- 4,096 visual fragments and shaped segments;
- four font metric records;
- mixed OS/2 typographic, hhea and negative-gap sources;
- four fragments per line at different retained HarfBuzz scales.

Every line resolves to 950 units of ascent, 350 units of descent and a 1,300-unit block size. Expected retained and peak PMR memory are both exactly 180,224 bytes.

## Explicit exclusions

Z2E-2 does not implement CSS `line-height` parsing, author-specified leading distribution, vertical-align variants, inline replaced elements, ruby, vertical writing modes, glyph ink bounds, selection geometry, painting, clipping, hit testing or rasterization.
