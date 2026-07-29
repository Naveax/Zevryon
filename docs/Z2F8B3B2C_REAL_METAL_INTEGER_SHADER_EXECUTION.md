# Z2F-8B3B2C: Real Metal Integer Shader Execution

## Scope

Z2F-8B3B2C binds the certified Z2F-8B3B1 `GpuShaderPacket` ABI to a
retained Metal device and command queue. The production path compiles the
integer composer into a build-time `.metallib`, retains persistent atlas
textures and shared buffers, executes ordered sparse compute dispatches, and
publishes a GPU readback that must match the B3B1 reference oracle byte for
byte.

This closes the third native shader backend after Direct3D 12 and Vulkan.

## Native execution graph

```text
Z2F-8B2C retained MTLDevice / MTLCommandQueue
        │
        ▼
build-time Metal library
        │
        ▼
MTLComputePipelineState
        ├── persistent R32Uint atlas texture array
        ├── persistent fill and glyph buffers
        ├── persistent BGRA output buffer
        └── integer dispatch constants
        │
        ▼
clear dispatch
        │
        ▼
selection → glyph → caret sparse dispatches
        │
        ▼
MTLBarrierScopeBuffers ordering
        │
        ▼
shared MTLBuffer readback
        │
        ▼
B3B1 reference oracle
```

## Exact integer composition

The Metal kernel reproduces the same byte-domain operations used by the CPU
reference, Direct3D 12 HLSL and Vulkan GLSL implementations:

- `mul_u8` with integer rounding;
- premultiplied source-over blending;
- Alpha8 coverage;
- independent LCD RGB coverage;
- premultiplied BGRA color-glyph modulation;
- nearest-neighbour atlas sampling.

Each visible instance is dispatched only over its surface- and scissor-clipped
rectangle. A buffer memory barrier separates ordered dispatches, preserving
selection, glyph and caret ordering without scanning every command for every
surface pixel.

## Retained ownership

The executor consumes the exact Z2F-8B2C context lease. It does not create a
second Metal device or command queue. The original owner may shut down after
context export; the executor remains valid until its retained context is
released.

## Persistent resources

- Canonical atlas pages are mirrored into one `MTLTextureType2DArray` with
  `MTLPixelFormatR32Uint`.
- Page index maps directly to texture-array slice.
- Generation, dimensions and canonical checksum determine native residency.
- Cold packets upload changed pages through `replaceRegion`.
- Hot packets reuse the existing texture and publish zero atlas uploads.
- Fill, glyph and output buffers grow only when required and are reused across
  frames.
- Output bytes are read directly from a shared Metal buffer after command-buffer
  completion; no CPU full-frame composition occurs.

## Certification topology

The fixed fixture remains identical across all three native backends:

- 68 ordered commands;
- 65 fill instances;
- 240 glyph instances;
- 3 persistent atlas pages;
- 49,152 resident atlas bytes;
- 640 × 360 output;
- 921,600 readback bytes;
- one cold atlas upload batch;
- hot atlas reuse for every measured execution;
- reference and GPU checksum `13606000133885175515`.

## Failure model

The executor fails closed for invalid packet checksum, stale context
generations, missing atlas pages, conflicting page generations, record spans,
surface and packet budget overflow, metallib or pipeline creation failure,
command encoder failure, command-buffer error and readback allocation failure.
Resource replacement is transactional and partial allocations remain ARC/RAII
clean.

## Explicit boundary

This slice certifies exact offscreen Metal compute execution and readback. The
next compositor step will bind shader output directly into the real
`CAMetalDrawable` presentation path and establish a three-backend native
present/readback equivalence gate without an intermediate CPU surface.
