# Z2F-8B3B2C: Real Metal Integer Shader Execution

## Scope

Z2F-8B3B2C binds the certified Z2F-8B3B1 `GpuShaderPacket` ABI to the retained
Z2F-8B2C Metal device and command queue. The production path no longer builds
or uploads a full CPU-composed frame. Selection, caret, Alpha8, LCD RGB and
BGRA glyph composition executes in an integer Metal compute kernel and the
result is copied to a shared `MTLBuffer` for exact comparison with the B3B1
reference oracle.

## Same-device execution graph

```text
Z2F-8B2C MTLDevice + MTLCommandQueue
        │ retained context lease
        ▼
Z2F-8B3B1 GpuShaderPacket
        ├── 68 ordered commands
        ├── 65 fill instances
        ├── 240 glyph instances
        └── 3 persistent atlas pages
        │
        ▼
MTLComputePipelineState
        ├── R32Uint 2D-array atlas texture
        ├── private R32Uint output texture
        ├── 96-byte integer constants
        └── ordered texture memory barriers
        │
        ▼
MTLBlitCommandEncoder
        │
        ▼
shared MTLBuffer readback
        │
        ▼
B3B1 reference oracle
```

The executor never creates a second `MTLDevice` or `MTLCommandQueue`. It
retains the exact Metal window context exported by Z2F-8B2C and remains valid
after the original owner shuts down.

## Exact integer composition

The embedded Metal Shading Language kernel reproduces the B3B1 byte-domain
operations directly:

- `mul_u8` with `+127 / 255` rounding;
- premultiplied source-over blending;
- Alpha8 coverage;
- independent LCD RGB coverage;
- premultiplied BGRA color glyph modulation;
- nearest-neighbour atlas sampling;
- selection → glyph → caret ordering.

One clear dispatch is followed by only the clipped primitive rectangles.
Texture memory barriers preserve packet order between dispatches.

## Persistent resources

Canonical pages are published into one persistent `MTLTextureType2DArray`
using `MTLPixelFormatR32Uint`. Page generation, dimensions and canonical pixel
checksum form the native residency signature. Cold packets upload changed
pages; hot packets reuse the existing texture and produce zero atlas uploads.

The private output texture and aligned shared readback buffer are reused while
the surface dimensions remain unchanged.

## Certification contract

The fixed fixture requires:

- 68 commands;
- 65 fill instances;
- 240 glyph instances;
- three resident atlas pages / 49,152 bytes;
- 640 × 360 output / 921,600 canonical bytes;
- one cold upload batch;
- hot atlas reuse;
- byte-for-byte GPU/reference equality;
- owner-shutdown retained-context execution;
- checksum, generation and budget failures to remain fail-closed.

## Explicit boundary

This slice certifies exact native Metal compute execution and readback. The
next cross-backend stage can remove certification readback from production,
batch instance data into persistent structured buffers, and feed D3D12,
Vulkan and Metal shader output directly into their window back buffers while
retaining the same packet ABI and pixel contract.
