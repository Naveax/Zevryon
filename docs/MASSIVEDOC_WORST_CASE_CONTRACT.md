# Zevryon MassiveDoc Worst-Case Contract

## Measurement unit

Message count is not a performance unit. A message can contain one byte or many gigabytes. Zevryon is therefore certified on independent load dimensions:

1. logical UTF-8 bytes;
2. Unicode scalar values and grapheme clusters;
3. words/tokens;
4. logical records;
5. DOM/logical nodes;
6. style runs;
7. resource references;
8. maximum individual record/token/grapheme stress.

The primary scale is **logical UTF-8 payload bytes**. Record count only describes fragmentation.

## Certified adversarial envelope

| Dimension | Required load |
|---|---:|
| Logical UTF-8 text | **4 GiB** |
| Logical records | **8,388,608** |
| Logical nodes | **67,108,864** |
| Style runs | **33,554,432** |
| Resource references | **1,048,576** |
| Largest individual record | **64 MiB** |
| Largest unbroken token | **16 MiB** |
| Pathological grapheme stress | **64 KiB** |

This envelope intentionally crosses the 32-bit offset boundary. All persistent indexes and logical positions must use 64-bit addressing. A 32-bit process must use windowed I/O rather than mapping or allocating the entire document.

A single message has no semantic size limit. Records larger than the hot-materialization limit are represented by streamed segments while copy/export preserves the exact original byte sequence.

## Survival envelope

The 100-point latency contract is finite, but input safety cannot stop at that boundary.

| Process architecture | Must open without crash/OOM |
|---|---:|
| 32-bit-safe mode | **16 GiB logical payload** |
| 64-bit mode | **64 GiB logical payload** |

Above the 4 GiB certification load, Zevryon may reduce cache size, search-index detail and prefetch distance. It may not corrupt data, truncate selection, emit invalid UTF-8, or terminate from memory pressure.

## Device profiles

| Profile | Minimum RAM | Target process-group PSS | Absolute survival cap | Scroll P99 | Warm exact search |
|---|---:|---:|---:|---:|---:|
| Legacy phone | 2 GiB | **64 MB** | **80 MB** | **33.3 ms** | **350 ms** |
| Mid phone | 4 GiB | **80 MB** | **96 MB** | **16.6 ms** | **180 ms** |
| Modern phone | 8 GiB | **96 MB** | **128 MB** | **11.1 ms** | **120 ms** |
| Desktop | 8 GiB+ | **128 MB** | **160 MB** | **8.33 ms** | **100 ms** |

Memory is measured in **decimal MB**: one MB is 1,000,000 bytes. The value is absolute aggregate PSS for all browser/document/renderer/search/runtime processes used by the benchmark. File-backed resident pages count. Empty-process cost cannot be subtracted to disguise overhead. Extra RAM may improve cache hit rates, but it does not relax the survival cap.

## Unicode adversarial classes

The corpus must contain all of the following simultaneously:

- ASCII one-character words to maximize word count per byte;
- Turkish dotted/dotless-I and normalization variants;
- Arabic and Hebrew bidirectional runs;
- CJK text without spaces;
- emoji ZWJ sequences and skin-tone modifiers;
- combining-mark chains;
- invalid UTF-8 at import boundaries, which must be rejected or losslessly quarantined;
- a 16 MiB unbroken token;
- a pathological grapheme sequence handled with bounded scratch memory.

A pathological grapheme may be rendered through a bounded fallback representation, but its source bytes and logical selection range must remain exact.

## 100/100 rule

There is no weighted compensation. A result is 100 only when every gate passes:

- certified load dimensions reached;
- device memory cap respected;
- first viewport and scroll latency respected;
- warm/cold search limits respected;
- mutation and copy throughput respected;
- zero crashes/OOMs;
- zero data loss;
- zero invalid UTF-8 output;
- exact full-document selection/copy/search semantics.

## Why no “average message” exists

An average hides the cases that destroy real systems: one giant message, millions of tiny records, extreme style fragmentation, huge combining sequences or an enormous unbroken token. MassiveDocBench therefore publishes all load dimensions and tests them as a combined adversarial envelope.
