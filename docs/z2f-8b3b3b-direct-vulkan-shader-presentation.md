# Z2F-8B3B3B — Direct Vulkan shader-surface presentation

## Purpose

Remove the production CPU full-frame readback/upload hop between the certified
Vulkan integer shader executor and the existing Vulkan window swapchain while
retaining GPU readback as an explicit certification mode.

This slice starts from `main` at:

```text
476527709aafd50b7950db8d9ec7d656acf57420
```

It is independent of the locked Z2R-3E-U authority-certification branch and must
not modify PR #91 or its exact-head evidence.

## Frozen input contract

The existing 72-byte `NativeShaderSurfaceView` remains the cross-backend ABI.
A Vulkan surface is accepted only when all of the following match the presenter:

- API kind is Vulkan;
- generic ready/non-owning/premultiplied flags are present;
- abstract state is `ShaderRead`;
- device generation;
- runtime generation;
- frame ID;
- command/content checksum;
- surface width and height;
- native image handle is non-null;
- the actual swapchain native format is `VK_FORMAT_B8G8R8A8_UNORM`.

A request carrying both a CPU pixel buffer and a GPU shader surface is rejected.
No backend fallback is permitted after a non-empty but invalid shader surface is
supplied.

## GPU-resident copy model

The Vulkan integer composer stores canonical packed BGRA bytes in one
`VK_FORMAT_R32_UINT` output image. The direct presentation path is enabled only
for a `VK_FORMAT_B8G8R8A8_UNORM` swapchain target. Both formats use one four-byte
texel block, so `vkCmdCopyImage` preserves the packed byte-domain payload without:

- host-visible readback;
- host staging upload;
- floating-point color conversion;
- a second GPU device or queue graph.

The source image transitions from `GENERAL` to `TRANSFER_SRC_OPTIMAL` and back
to `GENERAL`. The acquired swapchain image transitions to
`TRANSFER_DST_OPTIMAL` and then to `PRESENT_SRC_KHR`.

## Executor direct mode

The Vulkan executor supports two explicit modes:

- certification mode: `ShaderReadback*` is non-null and the completed image is
  copied to the bounded host-visible readback buffer;
- production direct mode: `ShaderReadback*` is null and no image-to-host copy is
  encoded.

Both modes execute the same integer-composition shader. A successful execution
publishes a non-owning `NativeShaderSurfaceView`; the snapshot advertises
`kNativeShaderExecutionDirectSurfaceExport`. Direct executions increment
`executions` but do not increment `readbacks`.

## Presenter integration

`VulkanNativeWindowSwapchainApi::present()` now has three mutually exclusive
render sources:

1. validated Vulkan shader surface — raw GPU image copy;
2. validated CPU pixel buffer — existing host staging upload;
3. neither — existing checksum-derived clear fallback.

The CPU staging path and checksum-clear fallback are retained unchanged. Invalid
or mixed shader-surface requests fail before command submission.

## Lifetime and ordering

- Shader execution and presentation use the retained Vulkan WSI context.
- Both submissions use the existing device mutex and graphics queue.
- The executor waits for its execution fence before returning an exported
  surface.
- Presentation copy submission therefore consumes a completed source image.
- Later executor submissions on the same graphics queue are ordered after the
  presentation copy submission.
- Output-image rebuild and executor shutdown continue to wait for device idle.
- Swapchain image and frame-slot ownership remain governed by the existing
  acquire/present/fence state machine.

## Test coverage in this slice

The source tree now contains:

- isolated shader-surface contract tests;
- Vulkan executor tests that exercise explicit readback and zero-readback direct
  execution in the same retained context;
- a real WSI integration test that configures executor and presenter from one
  native Vulkan graph, shuts down the owner, presents direct surfaces, rejects a
  mixed CPU/GPU request, rejects a stale runtime generation and verifies the
  executor readback count remains zero.

These tests are source-complete but are not certification evidence until they
are compiled and executed on a host with the required Vulkan SDK/runtime and a
real supported Win32/XCB/Wayland WSI path.

## Remaining validation

- compile the exact PR head with warnings treated as errors;
- execute the Vulkan surface contract test;
- execute existing real Vulkan shader and WSI lifecycle tests;
- execute the new direct-surface integration test on supported native WSI;
- add byte/pixel-equivalence evidence for the presented image rather than only
  command/lifecycle evidence;
- add resize/direct-surface regression coverage if the native run exposes an
  ordering or generation gap;
- collect direct-present latency and compare it with the readback/upload path;
- run sanitizer/validation-layer diagnostics where supported.

## Explicit boundary

The production code path is implemented, but this slice is not merge-ready.
Native compilation, real WSI execution, pixel-equivalence evidence and
performance/regression certification remain mandatory. PR #93 must stay draft
until those gates close.