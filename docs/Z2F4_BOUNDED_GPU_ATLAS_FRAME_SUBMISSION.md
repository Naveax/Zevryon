# Z2F-4: Bounded GPU Atlas Residency and Frame Submission

## Scope

Z2F-4 consumes the certified Z2F-1 paint stream, Z2F-2 atlas draw topology,
and Z2F-3 upload execution. It publishes one backend-neutral frame command
stream, synchronizes uploaded atlas pages into a bounded GPU-residency model,
pins every sampled page for the lifetime of an in-flight frame, and retires
those pins only after a monotone completion fence.

The stage is bounded by the projected viewport command stream, referenced atlas
pages and configured in-flight frame ring. Total document size and total font
glyph count do not affect its retained work.

## Frame preparation

`prepare_gpu_frame_submission` verifies that the Z2F-2 submission and Z2F-3
upload execution are still current, then publishes:

- one copied viewport clip table;
- selection fill commands first;
- one zero-copy glyph range command per atlas draw batch;
- caret fill commands last;
- one unique page reference per sampled atlas page;
- the maximum upload fence required before drawing.

Glyph instances remain owned by `GlyphAtlasSubmission`; Z2F-4 stores only
`first_instance` and `instance_count`. No glyph geometry, atlas pixels or font
bytes are copied.

Page references preserve the exact atlas generation, page generation, raster
format and required upload fence. Their `first_batch` and `batch_count` values
are checked against every referring glyph batch, including non-contiguous batch
occurrences. Duplicate page identities fail closed.

## Paint order

The command partition is fixed:

1. selection fill rectangles;
2. atlas glyph batches;
3. active caret fill rectangles.

Each command retains a validated clip index. Glyph batches retain style, clip,
page generation and the source draw-instance range.

## GPU residency scheduler

`GpuAtlasFrameScheduler` owns persistent metadata charged to
`ResourceClass::CompositorSurface`:

- a fixed-capacity resident atlas-page table;
- a fixed-capacity in-flight frame ring;
- a fixed-capacity page-pin table;
- scheduler and atlas generation IDs;
- monotone submit and completed fence values.

Cold upload receipts synchronize resident page generation, format and ready
fence. A hot frame may reuse a page only when the exact generation and format
are already resident. Replacing a page with a different generation or format is
forbidden while any in-flight frame pins it.

An atlas-generation transition clears the resident page table only when no
frame or page pin remains. Scheduler `clear()` has the same requirement and
increments the scheduler generation so previously issued receipts become stale.

## Transactional submission

`submit_gpu_frame` stages candidate page, frame and pin tables in the bounded
metadata resource before calling the backend. Publication occurs only after:

- frame and atlas generations are current;
- scheduler capacities are available;
- upload receipts synchronize all cold pages;
- every frame page is resident and ready;
- page replacement is not blocked by a pin;
- the backend returns a strictly increasing fence;
- the frame remains current after backend submission.

A failed publication leaves scheduler metadata unchanged. A backend that has
already accepted work may produce an orphaned fence when a concurrent atlas
change makes the final currentness check fail; that fence cannot be referenced
by scheduler state and therefore cannot make stale pages drawable.

## Retirement

`retire_gpu_frames` accepts a monotone completed fence and retires the maximal
in-flight prefix whose submit fences are complete. It first validates the full
frame/pin prefix without mutation, then decrements page pins and erases the
retired records. This preserves failure atomicity for corrupted pin topology.

Pages become replaceable only after all pins referencing their generation have
been retired.

## Backend interface

`GpuFrameBackend` receives:

- the immutable frame command stream;
- a zero-copy span of Z2F-2 draw instances;
- a monotone ticket ID;
- the upload fence that must be waited on.

It returns one signal fence. `ReferenceGpuFrameBackend` is a deterministic CPU
validation backend used to certify command, range and fence invariants. Vulkan,
Metal and Direct3D adapters can implement the same interface without changing
the upstream layout/raster contracts.

## Compact records

| Record | Bytes |
|---|---:|
| `GpuSurfaceDescriptor` | 32 |
| `GpuFrameCommandRecord` | 16 |
| `GpuFrameGlyphBatch` | 40 |
| `GpuFramePageReference` | 32 |
| `GpuAtlasResidentPage` | 48 |
| `GpuFramePagePin` | 32 |
| `GpuInFlightFrameRecord` | 56 |
| `GpuFrameReceipt` | 56 |

## Failure model

The stage fails closed for:

- stale Z2F-2/Z2F-3 generation state;
- malformed paint partitions;
- malformed draw-instance partitions;
- page format or generation disagreement;
- missing upload completion;
- duplicate or inconsistent page references;
- output, resident-page, frame-ring or pin limits;
- sampling a non-resident or not-ready page;
- replacing a pinned page;
- non-monotone backend or completion fences;
- corrupted frame/pin topology;
- PMR metadata/output exhaustion;
- checked arithmetic overflow.

Frame outputs and scheduler metadata remain unchanged after every pre-publication
failure.

## Certification fixture

The fixed benchmark represents:

- a 16,384-line source document;
- an 80-line viewport projection;
- 145 input paint commands;
- 310 non-empty atlas draw instances;
- 64 selection rectangles and one caret;
- three raster-format atlas pages;
- one cold frame waiting on Z2F-3 uploads;
- one hot frame reusing the same resident pages with zero upload wait;
- submit and retirement of both frames;
- zero remaining in-flight frames and page pins;
- exact output and scheduler PMR accounting;
- deterministic checksum across hosted distributions.

The independent oracle covers 9,216 cold/hot combinations of selection and
caret counts, paint glyph placeholders, clip tables, surface identities, exact
versus slack output limits and frame IDs.

## Explicit boundary

Z2F-4 does not allocate real Vulkan, Metal or Direct3D textures; encode native
command buffers; build blend pipelines; perform color management; track window
occlusion or damage; composite images and layers; acquire swapchain images;
wait on operating-system presentation fences; or present a final frame. Those
belong to the compositor-surface and platform GPU adapter stages.
