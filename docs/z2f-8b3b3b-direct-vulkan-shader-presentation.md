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
- native image handle is non-null.

A request carrying both a CPU pixel buffer and a GPU shader surface is rejected.
No backend fallback is permitted after a non-empty but invalid shader surface is
supplied.

## GPU-resident copy model

The Vulkan integer composer stores canonical packed BGRA bytes in one
`VK_FORMAT_R32_UINT` output image. The swapchain uses a four-byte BGRA8 texel.
The direct path records a raw `vkCmdCopyImage` operation so the byte-domain
output is preserved without:

- host-visible readback;
- host staging upload;
- floating-point color conversion;
- a second GPU device or queue graph.

The source image transitions from `GENERAL` to `TRANSFER_SRC_OPTIMAL` and back
to `GENERAL`. The acquired swapchain image transitions to
`TRANSFER_DST_OPTIMAL` and then to `PRESENT_SRC_KHR`.

## Lifetime and ordering

- Shader execution and presentation use the retained Vulkan WSI context.
- Both submissions use the existing device mutex and graphics queue.
- Queue submission order protects the shared output image from being overwritten
  before the presentation copy consumes it.
- Output-image rebuild and executor shutdown continue to wait for device idle.
- Swapchain image and frame-slot ownership remain governed by the existing
  acquire/present/fence state machine.

## Initial foundation

The first commit series adds:

- backend-specific surface decoding and native handle conversion;
- raw image-copy command encoding with explicit barriers;
- strict negative contract tests;
- an isolated CMake probe that does not modify the production graph.

## Remaining implementation

- allow the Vulkan executor to execute with `readback == nullptr`;
- publish `kNativeShaderExecutionDirectSurfaceExport` capability;
- populate and export the completed Vulkan `NativeShaderSurfaceView`;
- integrate the helper into `VulkanNativeWindowSwapchainApi::present()`;
- retain the existing CPU pixel-buffer and checksum-clear paths;
- add real XCB/Wayland lifecycle and pixel-equivalence tests;
- add owner-shutdown, stale-generation, mixed-source and resize tests;
- measure production direct-present latency and prove `readbacks == 0`;
- retain an independent readback execution for byte-exact certification.

## Explicit boundary

This foundation is not yet a completed native presentation path. It does not
claim production integration, real WSI execution, zero readbacks, performance
certification or merge readiness until the remaining implementation and tests
are complete.
