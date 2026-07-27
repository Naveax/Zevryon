# Z2F-6: Bounded Native Damage and Swapchain Presentation

## Scope

Z2F-6 consumes the certified Z2F-4 `GpuFrameSubmission`, Z2F-2 draw-instance
spans and the Z2F-5 device/surface identity model. It computes a bounded damage
region, emits a backend-neutral native render command buffer with exact
scissors, and schedules acquire/encode/present work through a bounded
swapchain-style frame ring.

The stage is bounded by viewport frame commands, active draw instances,
configured damage limits and frames in flight. Total document size does not
participate in its work or retained metadata.

## Command footprints and damage

Each frame command is converted to a compact `NativeCommandFootprint`:

- fill rectangles retain their exact viewport bounds and style checksum;
- glyph batches retain the union of their referenced atlas draw instances;
- every footprint is intersected with the certified paint clip and surface;
- zero-area or fully clipped commands are retained as empty history entries;
- glyph geometry remains owned by the Z2F-2 atlas submission.

A new frame is compared with the immediately preceding command generation.
Changed commands damage both their old and new bounds. Removed commands damage
their old bounds so stale pixels are cleared. Callers may append explicit
invalidations for non-command state such as exposure or window-system damage.

The damage planner:

- clips every rectangle to the surface;
- optionally merges touching rectangles;
- repeatedly merges overlap chains to a stable region;
- collapses to one union rectangle when the configured count overflows;
- optionally promotes high-area partial damage to one full-surface redraw;
- fails closed on coordinate, area or caller-limit overflow.

## Native command stream

The emitted stream is fixed and deterministic:

1. begin render pass;
2. set one scissor for each disjoint damage rectangle;
3. replay only intersecting frame commands in original paint order;
4. end render pass.

Fill and glyph commands reference the original Z2F-4 payload indices. No fill
rectangles, glyph batches, draw instances, texture pixels or font data are
copied. A frame whose command footprints and explicit invalidations are
unchanged emits zero damage and zero native commands.

## Swapchain and presentation scheduling

`NativePresentationScheduler` owns persistent metadata charged to
`ResourceClass::CompositorSurface`:

- a fixed-capacity in-flight frame ring;
- one scheduler generation;
- one globally monotone native fence timeline;
- configured surface identity and generation;
- present-mode and backpressure policy;
- submitted, skipped, dropped, retired and recovery counters.

`NativeGpuCommandApi` is the adapter boundary for Vulkan, Metal and Direct3D:

1. configure or recreate a surface image ring;
2. acquire the next image under FIFO, Mailbox or Immediate policy;
3. encode the bounded command stream and referenced Z2F-4 payloads;
4. submit and present with one monotone signal fence.

`ReferenceNativeGpuCommandApi` supplies deterministic image identities,
command checksums and fences. It also exposes one-shot out-of-date and
device-lost statuses for certification.

## Backpressure and recovery

- FIFO rejects submission while the bounded frame ring is full.
- Mailbox and configured drop mode publish a deterministic dropped receipt
  without encoding or presenting work.
- no-damage frames publish a skipped receipt without acquiring an image.
- surface reconfiguration is forbidden while frames remain in flight.
- acquired images must match device, surface and surface-generation identity.
- out-of-date and device-lost statuses are recoverable fail-closed results.
- completed fences may not regress or exceed submitted work.
- scheduler clear is forbidden while native frames remain in flight and
  invalidates earlier receipts by incrementing scheduler generation.

## Compact records

| Record | Bytes |
|---|---:|
| `NativeDamagePolicy` | 32 |
| `NativeDamageRect` | 32 |
| `NativeCommandFootprint` | 56 |
| `NativeCommandRecord` | 16 |
| `NativeSwapchainImageHandle` | 40 |
| `NativePresentReceipt` | 88 |
| `NativeInFlightFrameRecord` | 96 |
| `NativePresentationConfig` | 32 |

## Certification fixture

The fixed benchmark represents a 16,384-line document and an 80-line projected
viewport:

- 68 Z2F-4 frame commands;
- 310 referenced glyph draw instances;
- cold frame: one full-surface damage rectangle and 71 native commands;
- hot identical frame: zero damage, zero commands and one skipped present;
- partial frame: four damage rectangles and 17 native commands;
- 58 source commands culled from the partial redraw;
- one command duplicated under two disjoint scissors;
- cold/partial present fences: 4 / 5;
- submitted/skipped/retired frames: 2 / 1 / 1;
- one native frame remains in flight;
- scheduler metadata current/peak: 288 / 288 bytes;
- cold/hot/partial logical retained bytes: 4,976 / 3,808 / 4,208;
- deterministic checksum: `4778015303960727539`.

The independent oracle covers 9,216 change-mask, merge-policy and invalidation
combinations.

## Explicit boundary

Z2F-6 fixes the native command and swapchain scheduling ABI but does not yet
call Vulkan, Metal or Direct3D; allocate an operating-system window surface;
encode platform render-pass objects; translate styles into native pipelines;
perform layer-tree composition; paint image/video/canvas content; query real
occlusion; wait on operating-system fences; or display pixels. Platform
adapters can now implement those operations behind `NativeGpuCommandApi`
without changing the bounded damage or scheduling contracts.
