# Z2F-4: Bounded GPU Texture Residency and Compositor Submission

## Scope

Z2F-4 consumes the certified Z2F-1 paint stream, Z2F-2 atlas submission, and
Z2F-3 upload receipts. It allocates bounded GPU texture handles for active atlas
pages, encodes frame-local texture uploads, converts selection/glyph/caret
records into compositor packets, and publishes generation-safe frame receipts.

The cost is proportional to active viewport atlas pages, projected draw batches,
paint fills, and the fixed in-flight frame ring. Total document size and total
font glyph count do not affect this stage.

## Backend boundary

`GpuCompositorBackend` is API independent and defines four operations:

1. allocate one texture for an atlas page and format;
2. release a texture handle;
3. encode one page upload command against immutable Z2F-2 payload spans;
4. submit one complete compositor frame and return a monotone fence.

The deterministic reference backend certifies handle identity, texture
validation, upload topology, frame ordering, and fence lifecycle. It does not
allocate Vulkan, Metal, or D3D resources.

## Texture residency cache

`GpuTextureResidencyCache` owns metadata charged to
`ResourceClass::CompositorSurface`:

- a fixed maximum texture count;
- a fixed page width and height;
- a fixed in-flight frame ring;
- one device generation;
- per-texture atlas and page generations;
- pending/resident state and GPU ready fence;
- LRU use epochs;
- allocation, reuse, eviction, release, submit, retire, and stale counters.

A texture identity includes device generation, texture generation, texture ID,
page index, and raster format. Atlas/page generation mismatches never alias an
existing texture.

## Pending to resident promotion

New textures are published as `Pending` with `ready_fence_value == 0`. Preparing
a frame does not make them resident. On real frame submission, the returned GPU
fence is attached to every texture uploaded by that frame. A later
`retire_completed_frames` call promotes only nonzero ready fences less than or
equal to the completed fence.

This separates the Z2F-3 raster/upload receipt from the actual GPU texture
completion fence. A prepared but never submitted frame cannot become resident.

## Safe bounded eviction

When texture capacity is full, Z2F-4 chooses the least-recently-used texture
that is not referenced by the current frame. Eviction is permitted only when
there are no in-flight frames. The old backend handle is released only after the
staged metadata and frame output have completed successfully.

If every resident page is required by the frame, or any frame is in flight, the
operation fails closed with `TextureCapacityExceeded`.

## Frame-local command stream

A frame retains:

- the viewport clip table;
- texture upload commands;
- glyph draw packets;
- selection and caret fill packets;
- one strict compositor command sequence.

Command order remains:

1. selection fills;
2. glyph draws;
3. caret fills.

Glyph packets retain Z2F-2 draw-instance ranges and generation-safe texture
handles. Pending textures set `kGpuGlyphDrawRequiresUploadWait`; resident
textures do not. Consecutive atlas draw batches remain backend-visible and are
not flattened into copied glyph data.

## Submission and in-flight ring

Before calling the backend, every draw and upload texture handle is revalidated
against the current cache. Backend submission cannot occur with stale handles.

A successful submission receives a strictly increasing frame ID and fence and
occupies one fixed ring slot. Ring exhaustion fails closed. Retirement releases
all slots whose fences have completed and promotes uploaded textures to
resident state.

`clear()` releases every backend texture, empties the ring, increments residency
and device generations, and invalidates all previously prepared frames.

## Compact records

| Record | Bytes |
|---|---:|
| `GpuTextureHandle` | 32 |
| `GpuTextureResidencyRecord` | 72 |
| `GpuTextureUploadCommand` | 72 |
| `GpuGlyphDrawPacket` | 64 |
| `GpuFillRectPacket` | 48 |
| `GpuCompositorCommandRecord` | 16 |
| `GpuFrameReceipt` | 48 |
| `GpuInFlightFrameRecord` | 64 |

## Failure atomicity

Preparation stages cache metadata and frame vectors independently. Newly
allocated backend handles are released after any validation, metadata, output,
or backend-encoding failure. Evicted handles remain live until the staged cache
is committed. Output remains empty after failure.

The stage fails closed for:

- stale atlas submissions or Z2F-3 upload executions;
- invalid upload/draw/fill topology;
- texture or in-flight capacity exhaustion;
- missing/stale texture handles;
- page-generation or device-generation mismatch;
- frame output limits;
- metadata/output PMR exhaustion;
- backend allocation, upload, or submit failure;
- arithmetic and fence overflow.

## Certification fixture

The fixed benchmark represents:

- 16,384 source-document lines;
- an 80-line projected viewport;
- 93 atlas upload records across three pages/formats;
- 310 glyph draw instances in three batches;
- 64 selection fills and one caret fill;
- cold frame: three GPU texture upload commands;
- warm frame: zero texture upload commands;
- three glyph draw packets, 65 fill packets, and 68 compositor commands;
- exact per-frame output current/peak: 4,648 / 4,652 bytes;
- deterministic checksum: `6193958397185634556`.

The independent oracle covers 192 pending/resident state and command-order
combinations. Focused tests additionally cover zero-fence promotion,
transactional output failure, stale-frame invalidation, in-flight ring capacity,
and safe LRU eviction.

## Explicit boundary

Z2F-4 does not yet create actual Vulkan/Metal/D3D textures, command pools,
descriptor sets, render passes, swapchains, window surfaces, presentation
queues, damage regions, image layers, CSS transforms, opacity groups, filters,
or multi-surface compositing. Those remain platform backend and broader paint
pipeline stages after this generation-safe contract.
