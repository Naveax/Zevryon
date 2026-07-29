# Z2F-8B3B2C: Real Metal Integer Shader Execution

## Scope

Z2F-8B3B2C binds the certified Z2F-8B3B1 `GpuShaderPacket` ABI to the retained
Metal device and command queue created by the existing Cocoa window owner. It
executes the same byte-domain composition contract already certified on D3D12
and Vulkan, reads the native GPU result back, and compares it byte-for-byte
against the B3B1 reference oracle.

No full CPU-composed 640×360 surface is uploaded to the GPU.

## Native execution graph

```text
Z2F-8A-compatible Metal window owner
MTLDevice + MTLCommandQueue
        │ retained context lease
        ▼
Embedded build-time metallib
        │
        ▼
MTLComputePipelineState
        ├── R32Uint output texture
        ├── R32Uint persistent atlas texture array
        ├── 96-byte integer constants
        └── ordered texture barriers
        │
        ▼
Managed output synchronization
        │
        ▼
MTLTexture getBytes readback
        │
        ▼
B3B1 reference oracle
```

## Deterministic shader build

The Metal source is compiled during the macOS build:

1. `xcrun metal` produces AIR;
2. `xcrun metallib` produces a native Metal library;
3. `tools/embed_binary.py` embeds the library as a fixed C++ byte array;
4. the executor loads it through `newLibraryWithData`.

Runtime source compilation is not used.

## Exact integer composition

The Metal kernel reproduces the shared byte-domain contract:

- selection and caret fills;
- Alpha8 glyph coverage;
- LCD RGB channel coverage;
- premultiplied BGRA color glyphs;
- nearest-neighbour atlas sampling;
- exact `mul_u8` rounding;
- exact premultiplied source-over blending.

The executor emits one clear dispatch and one clipped dispatch for each visible
fill or glyph instance. Texture barriers preserve packet ordering.

## Persistent resources

Canonical atlas pages are mirrored into one managed `MTLTextureType2DArray`
with `MTLPixelFormatR32Uint`:

- page index maps directly to the texture array slice;
- page generation, dimensions and canonical checksum form the residency
  signature;
- cold packets rebuild changed residency;
- hot packets reuse the existing native texture and issue zero uploads.

The output `R32Uint` texture is also retained while surface dimensions remain
unchanged. After command-buffer completion, a blit synchronization makes the
managed texture visible to the CPU readback path.

## Single-device invariant

The executor does not create another Metal device or command queue. It retains
the exact `MetalWindowSharedContext` exported by the owner. The owner may shut
down immediately after executor configuration; the executor remains valid
until its own lease is released.

## Certification topology

- surface: 640×360;
- commands: 68;
- fill instances: 65;
- glyph instances: 240;
- atlas pages: 3;
- persistent atlas bytes: 49,152;
- output/readback bytes: 921,600;
- cold atlas upload batches: 1;
- reference checksum: `13606000133885175515`.

Cold and hot native readbacks must match the reference oracle byte-for-byte.

## Explicit boundary

This stage completes real integer shader execution on D3D12, Vulkan and Metal.
The current sparse topology intentionally dispatches per visible instance to
preserve exact ordering. Future optimization may introduce structured instance
buffers, indirect dispatch, tile binning and direct rendering into window
back-buffers without changing the packet ABI or pixel contract.
