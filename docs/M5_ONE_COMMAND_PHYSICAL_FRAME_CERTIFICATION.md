# M5 — One-Command Physical Frame Certification

## Purpose

`prepare_m5_physical_frame.py` is the cross-platform physical certification entry point for the native M5 frame gate. It deliberately reuses the repository's existing corpus generator, MassiveDoc CLI, native frame probe, and certification evaluator instead of inventing a parallel benchmark format.

## Workload

The default workload is deterministic and checkpoint-cheap:

- 64 MiB giant record at physical record index 0;
- one trailing 1-byte record because giant-record corpus generation requires at least two records;
- arena parameters: 96 estimated bytes per line and 18 px line height;
- persistent checkpoint: record 0, benchmark viewport width, 16 KiB stride;
- native frame probe: 2000 retained samples after 120 warmup observations by default.

The 18 px scroll step over 2000 samples remains deep inside the giant first record, so one physical checkpoint is sufficient for this workload. The probe itself still fails if any observation escapes the checkpoint path.

## Pipeline

Unless `--skip-build` is supplied, one invocation performs:

1. CMake Release configure;
2. Release build of `zevryon-massivedoc` and `zevryon-zenith-frame-probe`;
3. deterministic corpus generation;
4. MassiveDoc import;
5. store verification;
6. compact arena build;
7. 16 KiB persistent checkpoint build for record 0;
8. native `ZenithTabRuntime` frame probe;
9. physical/thermal frame-latency certification;
10. evidence manifest with SHA-256 bindings.

Windows multi-config (`Release/*.exe`) and normal single-config binary layouts are both resolved without shell-specific scripts.

## Fail-closed physical evidence

The top-level harness requires:

- `ZEVRYON_PHYSICAL_DEVICE=1`;
- `ZEVRYON_THERMAL_STATE=nominal` or `fair`;
- at least 1000 retained frame samples.

The lower-level certification evaluator still remains authoritative for canonical RAM-derived device profile and P99/max-stall limits. The harness always invokes it with `--require-pass`.

A certification artifact can therefore be written even when a gate fails, but the top-level command returns non-zero unless `checks.native_frame_certified` is true.

## Artifacts

The work directory contains:

- `frame.samples.txt`: raw post-warmup frame samples;
- `frame-certification.json`: canonical native certification evidence;
- `manifest.json`: workload definition, generator/import/verify/arena/checkpoint summaries, command stage return codes, and SHA-256 bindings for corpus/samples/evidence.

By default the large generated corpus/store are removed only after a manifest exists. `--keep-work-dir` retains the full prepared workload.

## Validation

Focused Python helpers cover:

- physical-device and thermal environment fail-closed behavior;
- Windows and single-config executable resolution;
- pretty JSON/final-line JSON extraction;
- safe work-directory reset protection;
- physical sample floor validation.

This script creates a reproducible way to obtain physical evidence. It is not itself evidence that any specific machine passed the gate.
