# M5 — Physical Frame Latency Evidence

## Purpose

Native device frame budgets are policy until they are backed by measured physical-device samples. This evidence layer turns raw frame latency samples into a deterministic certification artifact tied to the existing canonical device profiles.

## Statistics

The evaluator records:

- sample count;
- nearest-rank P50, P95 and P99 latency;
- maximum observed frame latency;
- counts above 50 ms and 75 ms;
- SHA-256 of a canonical serialization of the raw samples.

Nearest-rank percentiles are intentionally conservative; certification does not use interpolation to make a boundary sample look better.

## Certification gates

A frame run is certified only when all of these hold:

1. at least 1000 frame samples exist;
2. physical-device metadata is complete;
3. thermal state is explicitly `nominal` or `fair`;
4. measured P99 is at or below the selected canonical profile `scroll_p99_ms`;
5. maximum frame latency is at or below that profile's `maximum_normal_stall_ms`.

The 50/75 ms counts are retained as evidence but are not labeled as JavaScript long-task proof. JS attribution requires a separate execution trace.

## Physical run

Prepare a text file containing one frame latency in milliseconds per line (a JSON array or comma-separated values are also accepted), then run from the repository root with physical-device and thermal metadata explicitly supplied or detected:

```text
ZEVRYON_PHYSICAL_DEVICE=1
ZEVRYON_THERMAL_STATE=nominal
ZEVRYON_BENCHMARK_RUN_LABEL=m5-desktop-frame-run
python scripts/evaluate_frame_latency.py --samples evidence/m5/frame-ms.txt --output evidence/m5/frame-latency.json --require-pass
```

The artifact embeds the same privacy-safe machine metadata used by the physical benchmark layer. Hostname and username are not added.

This layer evaluates evidence; it does not fabricate raw timing samples. A subsequent benchmark-probe slice owns deterministic collection of those samples from the native runtime.
