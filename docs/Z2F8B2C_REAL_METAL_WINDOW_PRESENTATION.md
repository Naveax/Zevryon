# Z2F-8B2C — Real Metal CAMetalLayer Window Presentation

Z2F-8B2C connects Zevryon's native window swapchain ABI to a real macOS
`CAMetalLayer`. The implementation does not create a second Metal device or
command queue. A dedicated Z2F-8A-compatible owner exports one retained
`MTLDevice`/`MTLCommandQueue` graph, and the presenter leases that exact graph
until its final drawable has retired.

## Native graph

```text
NSWindow / NSView
        │
        ▼
CAMetalLayer
        │
        │ nextDrawable
        ▼
id<CAMetalDrawable>
        │
        ▼
id<MTLTexture>
        │
        ▼
MTLRenderPassDescriptor
        │
        ▼
id<MTLCommandBuffer>
        │
        ├── presentDrawable
        └── commit
```

## Ownership contract

`MetalWindowSharedContext` retains:

- the original `MTLDevice`;
- the original `MTLCommandQueue`;
- the caller-owned `CAMetalLayer`;
- device, runtime and window generations.

The presenter increments the private lease before configuration. The owner may
then shut down immediately; the Metal graph remains alive until the presenter
waits for all command buffers, releases every drawable and drops the final
lease.

## Presentation behavior

- `CAMetalLayer.device` is the exported device;
- `nextDrawable` performs real drawable acquisition;
- the drawable texture is used as a real render target;
- the checksum-derived clear is encoded into a Metal render pass;
- `presentDrawable` and `commit` perform real window presentation;
- FIFO maps to `displaySyncEnabled = YES`;
- Immediate maps to `displaySyncEnabled = NO` when available;
- Mailbox and explicit tearing fail closed;
- `maximumDrawableCount` is bounded to two or three images;
- two frames in flight are enforced;
- a nil drawable is reported as occlusion/drawable starvation.

## Resize and backing scale

A logical resize or backing-scale transition must advance the
`GpuSurfaceDescriptor` generation. Recreation waits for outstanding command
buffers, releases old drawable generations, updates `drawableSize`, and rejects
all pre-recreation image leases.

The surface width and height are physical drawable pixels. This keeps retina
backing-scale changes deterministic without embedding AppKit coordinates in the
stable swapchain ABI.

## Safety invariants

- no parallel Metal device or queue;
- owner shutdown cannot invalidate a configured presenter;
- stale device, runtime, surface, swapchain and drawable generations fail closed;
- no-damage drawables are released without a fake GPU submission;
- partial resource construction is ARC/RAII-clean;
- command-buffer errors transition the presenter to device-lost state;
- shutdown waits for every committed command buffer before dropping the lease.

## Explicit boundary

This stage clears and presents a real drawable texture. Selection, glyph, caret,
atlas upload, sampling and blend pipelines remain Z2F-8B3.
