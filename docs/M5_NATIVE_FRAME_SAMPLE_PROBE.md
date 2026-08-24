# M5 — Native Zenith Frame Sample Probe

## Purpose

This probe measures the complete native `ZenithTabRuntime::layout()` call path with `std::chrono::steady_clock`, rather than measuring only `ZenithHotScrollSession` internals. The measurement therefore includes ready-prefetch draining, visible layout, scheduler accounting and post-layout prefetch scheduling performed inside the runtime call.

Input/activity updates used to reverse scroll direction are deliberately outside the measured interval. They are input-dispatch work, not the frame-layout gate certified here.

## Sampling contract

`FrameLatencySampleCollector` separates warmup observations from retained measurements and keeps a bounded vector of nanosecond samples. Both warmup and retained sample counts are capped at 1,000,000 so benchmark instrumentation cannot grow without bound. This is an instrumentation memory limit, not a browser tab/session limit.

The probe emits one decimal millisecond value per line. That format is consumed directly by `scripts/evaluate_frame_latency.py` and the physical frame-latency evidence contract.

## Runtime path

The probe constructs one process-shared `SharedRecordLengthAuthority`, one bounded `SharedSourcePrefetchPool`, and one `ZenithTabRuntime` using the selected canonical device frame profile. The same authority is supplied to both runtime and pool, so cache-hit and worker-side cold record-bound paths are exercised rather than bypassed.

The scroll workload bounces deterministically between the start and end of the scrollable range. Velocity is derived from step distance and the profile frame budget. Every measured layout must remain on the checkpoint path and return at least one fragment or the probe fails closed.

## Usage

```text
zevryon-zenith-frame-probe <store-dir> <profile> <sample-output> <samples> [warmup=120] [width-px=1440] [height-px=900] [overscan-px=720] [max-fragments=512] [step-px=18]
```

Canonical profiles are `legacy-phone`, `mid-phone`, `modern-phone`, and `desktop`.

A physical certification run should retain at least 1000 post-warmup samples and then pass the generated sample file to the physical frame-latency evaluator together with explicit physical-device and thermal metadata.

## Validation

The collector and probe source were syntax/behavior checked locally with C++20 and `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`. Full repository Windows/Linux CI remains a separate evidence gate.
