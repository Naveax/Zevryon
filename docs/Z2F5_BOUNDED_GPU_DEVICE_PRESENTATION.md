# Z2F-5: Bounded GPU Device Texture and Surface Presentation

## Scope

Z2F-5 connects the certified Z2F-3 upload batches and Z2F-4 frame submissions
to a platform-neutral GPU device boundary. It owns bounded device texture
metadata, a bounded surface-image ring, in-flight frame receipts, texture pins,
and monotone upload/present fence validation.

The stage is bounded by active atlas pages, configured surface images and
in-flight viewport frames. Total document size does not affect its persistent
or per-submission work.

## Device API boundary

`GpuDeviceApi` exposes the minimal contract needed by future Vulkan, Metal and
Direct3D adapters:

1. create one texture for an atlas page and raster format;
2. upload one certified Z2F-3 atlas batch into that texture;
3. configure a generation-identified presentation surface and its image ring;
4. submit one Z2F-4 frame and present one acquired surface image;
5. release texture handles when evicted or the device generation is cleared.

`ReferenceGpuDeviceApi` is a deterministic CPU certification backend. It
creates synthetic handles, validates upload ranges and frame command topology,
computes deterministic payload/command checksums and produces one strictly
monotone fence timeline. It does not rasterize or display pixels.

## Texture residency

`GpuDevicePresentationBackend` implements both `GlyphAtlasUploadBackend` and
`GpuFrameBackend` so the same device timeline consumes Z2F-3 uploads and Z2F-4
frames.

Each resident texture records:

- device and texture generations;
- atlas and page generations;
- page index and raster format;
- upload-ready fence;
- last frame-use fence;
- payload checksum;
- pin count and pending/resident state.

Cold atlas pages allocate textures. Capacity pressure chooses the least-recently
used unpinned texture. Every texture referenced by an incomplete frame is pinned,
so it cannot be replaced or uploaded in place. A page becomes resident only
after its upload fence is reported complete.

## Surface and frame ring

The backend owns a fixed surface-image count and maximum in-flight frame count.
Surface identity includes both `surface_id` and generation. Reconfiguration is
rejected while frames are in flight and published atomically only after the
device API returns every image handle successfully.

Frame submission:

- acquires one non-in-flight surface image;
- validates every Z2F-4 page reference against one current device texture;
- rejects textures whose upload fence is not covered by the frame wait fence;
- deduplicates and pins referenced textures;
- preserves Z2F-4 selection -> glyph -> caret command order;
- submits and presents through the device API;
- records an immutable receipt and one in-flight frame entry.

Retirement is an atomic monotone prefix operation. Completed frames release
their surface images and texture pins. Completion fences may neither regress
nor advance beyond the latest submitted device fence.

## Global fence contract

Upload and present operations share one backend-visible timeline. Every new
fence must be strictly greater than:

- the last submitted upload or present fence;
- the completed fence;
- and, for frame presentation, the requested wait fence.

This prevents a platform adapter from publishing a locally valid but globally
regressing fence after another upload or present call.

## Compact records

| Record | Bytes |
|---|---:|
| `GpuDeviceTextureHandle` | 32 |
| `GpuSurfaceImageHandle` | 32 |
| `GpuDeviceTextureRecord` | 80 |
| `GpuSurfaceImageRecord` | 64 |
| `GpuDeviceTexturePin` | 32 |
| `GpuPresentReceipt` | 80 |
| `GpuDeviceInFlightFrameRecord` | 96 |

## Failure model

The backend fails closed for:

- invalid device, texture, page or surface generations;
- malformed upload ranges or payload spans;
- duplicate upload into a pinned texture;
- texture exhaustion when every replacement candidate is pinned;
- surface reconfiguration while frames are in flight;
- exhausted surface images, frame slots or pin capacity;
- missing or not-ready page textures;
- invalid selection/glyph/caret command topology;
- non-monotone upload, present or completion fences;
- stale texture pin or surface-image identity during retirement;
- metadata hard-limit exhaustion.

Persistent frame, pin and surface state is published only after backend success.
New texture handles are released when upload publication fails.

## Certification fixture

The fixed fixture represents the existing Z2F viewport chain:

- source document: 16,384 lines;
- projected viewport: 80 lines;
- Z2F-3 uploads / payload: 93 / 12,720 bytes;
- Z2F-2 draw instances / batches: 310 / 3;
- selection / caret commands: 64 / 1;
- device textures: 3;
- surface images: 3;
- cold upload fences: 1, 2 and 3;
- cold present fence: 4;
- hot present fence: 5 with no texture upload;
- retained in-flight frame / texture pins after the hot frame: 1 / 3;
- metadata current / peak / hard limit: 816 / 1,104 / 1,104 bytes;
- deterministic checksum: `13139880764296641394`.

The independent oracle covers 9,216 combinations of texture count, surface
image count, fill partitions, generations, surface descriptors, payload bytes
and command geometry.

## Explicit boundary

Z2F-5 defines and certifies the device API but does not yet call Vulkan, Metal
or Direct3D; create a native swapchain; wait on operating-system fence objects;
track damaged rectangles; build a layer tree; composite images, video, canvas
or nested surfaces; perform color-space conversion; synchronize with a browser
UI process; or present pixels to a real window. Those remain subsequent Z2F
compositor and platform-adapter stages.
