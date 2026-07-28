# Z2F-8B3A — Bounded Shared Pixel Composition and Native Texture Transfer

## Scope

Z2F-8B3A converts the certified Z2F-4 selection/glyph/caret frame stream and
Z2F-2/Z2F-3 atlas data into deterministic premultiplied RGBA/BGRA pixels, then
transfers those pixels into the real D3D12, Vulkan and Metal window back buffers
introduced by Z2F-8B2A/B/C.

The work remains bounded by the active viewport frame, configured atlas pages,
and one output surface. Document length never participates in composition cost.

## Shared pixel contract

`NativeWindowPixelBufferView` is a read-only, generation-scoped view containing:

- exact width and height;
- exact row stride;
- RGBA8 or BGRA8 format;
- premultiplied-alpha declaration;
- immutable bytes and deterministic checksum.

A non-empty view must match the configured surface exactly. Invalid dimensions,
stride, byte count, format, or alpha mode fail closed before native submission.

## Deterministic composer

The common compositor implements:

- strict selection → glyph → caret order;
- viewport and command clip intersection;
- immutable sorted style handles;
- premultiplied source-over blending;
- Alpha8 glyph coverage;
- LCD RGB per-channel coverage;
- BGRA color-glyph sampling;
- cold atlas upload publication and hot retained-page reuse;
- atlas/page generation validation;
- failure-atomic output publication;
- exact surface and atlas byte limits.

Atlas pages are retained as a canonical four-byte texel representation so all
three native backends consume identical final pixels.

## Native transfer

- D3D12: upload heap, aligned placed footprint, `CopyTextureRegion`, present.
- Vulkan: host-visible coherent staging buffer, `vkCmdCopyBufferToImage`, present.
- Metal: shared `MTLBuffer`, blit encoder, drawable texture, `presentDrawable`.

The existing checksum-clear path remains only as a compatibility fallback for an
empty pixel view. Real certification benchmarks provide a non-empty full-surface
buffer.

## Explicit boundary

Z2F-8B3A proves correct common pixel composition and real native texture transfer.
Z2F-8B3B will replace the CPU-composed transfer surface with shader-native
instanced fills, atlas sampling, scissoring, and backend pixel-equivalence tests.
