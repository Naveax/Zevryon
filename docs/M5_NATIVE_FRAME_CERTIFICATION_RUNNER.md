# M5 — Native Frame Certification Runner

`certify_native_frame_latency.py` closes the evidence chain between the native runtime probe and the existing physical frame-latency gate.

## Canonical flow

1. Capture machine metadata and thermal observation.
2. Select the canonical device profile from physical RAM. The profile is not accepted as a user-supplied override.
3. Run `zevryon-zenith-frame-probe` with that profile.
4. Parse the probe's exact JSON summary contract.
5. Parse the raw post-warmup frame samples.
6. Evaluate P50/P95/P99/maximum latency against the canonical profile.
7. Combine probe contract checks and physical frame evidence into one deterministic JSON document.

The final `native_frame_certified` check requires all of the following:

- the probe operation identity is exact;
- probe profile matches the machine-derived profile;
- recorded and warmup sample counts match the requested run;
- visible layout count covers the complete warmup plus measured run;
- shared pool thread starts remain within the hard worker bound;
- the underlying physical/thermal frame-latency evidence is certified.

## Usage

```text
python scripts/certify_native_frame_latency.py \
  --probe build/zevryon-zenith-frame-probe \
  --store <prepared-store> \
  --output evidence/m5/native-frame.json \
  --samples 2000 \
  --warmup 120 \
  --require-pass
```

Unless `--samples-output` is supplied, raw samples are preserved beside the evidence JSON as `<output-stem>.samples.txt`. The evidence contains the canonical sample SHA-256 from the frame-latency layer, so the raw sample file can be independently checked against the published summary.

A run without explicit physical-device confirmation or a stable thermal state can still emit failed evidence, but it cannot become certified. `--require-pass` turns that failed evidence into a non-zero command result.

The certification parser is intentionally strict: extra probe JSON fields, missing fields, negative counters, profile mismatches, or insufficient visible-layout coverage fail closed rather than being silently ignored.
