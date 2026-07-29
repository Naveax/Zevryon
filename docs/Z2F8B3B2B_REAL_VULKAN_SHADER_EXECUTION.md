# Z2F-8B3B2B — Real Vulkan Integer Shader Execution

## Scope

Z2F-8B3B2B binds the backend-independent Z2F-8B3B1 `GpuShaderPacket` to a
real Vulkan compute pipeline. It removes Z2F-8B3A full-frame CPU composition
from the Vulkan path and preserves the CPU executor only as an exact pixel
oracle.

The executor retains the existing Z2F-8B2B `VulkanWsiSharedContext`. It does
not create another `VkInstance`, `VkPhysicalDevice`, `VkDevice`, or graphics
queue. The owner may shut down after executor configuration; the retained
context remains valid until the executor releases its final lease.

## Native graph

```text
Z2F-8B2B retained VkDevice + graphics VkQueue
        │
        ▼
Z2F-8B3B1 GpuShaderPacket
        ├── 68 ordered commands
        ├── 65 fill instances
        ├── 240 glyph instances
        └── 3 canonical atlas pages
        │
        ▼
Vulkan compute pipeline
        ├── embedded SPIR-V generated from GLSL 450
        ├── storage-image descriptor set
        ├── 96-byte push-constant ABI
        ├── R32_UINT persistent atlas image array
        ├── R32_UINT output storage image
        └── ordered compute barriers
        │
        ▼
VkBuffer mapped readback
        │
        ▼
B3B1 reference-oracle byte comparison
```

## Integer composition

The GLSL compute shader reproduces the reference executor's byte-domain
operations:

- selection and caret fills;
- Alpha8 glyph coverage;
- LCD RGB channel coverage;
- premultiplied BGRA color glyphs;
- nearest-neighbour atlas sampling;
- exact integer `mul_u8` and source-over blending.

Each dispatch covers only the clipped destination rectangle of one packet
instance. A compute-to-compute memory barrier preserves command order. This is
the Vulkan equivalent of the sparse D3D12 execution introduced by Z2F-8B3B2A.

## Persistent resources

Canonical atlas pages are published into one persistent
`VkImage`/`VkImageView` with `VK_FORMAT_R32_UINT` and array layers matching
stable page indices. Page generation, dimensions, and canonical checksum form
the native residency signature. Cold packets upload changed pages through a
bounded host-visible staging buffer; hot packets reuse the existing image and
issue zero atlas uploads.

The output storage image and mapped readback buffer are reused while the
surface dimensions remain unchanged. Descriptor set, pipeline layout, compute
pipeline, command pool, command buffer, and fence are created once per
executor configuration.

## Transaction and failure model

The executor rejects mutated packet checksums, invalid command ranges, invalid
scissors, missing or mixed atlas generations, resource-budget overflow, stale
native context, submission failures, fence timeout, and mapped-readback
failure. Native output is published only after the queue fence completes.
Failed submissions invalidate the affected output allocation instead of
publishing speculative layout state.

## Certification

The focused workflow runs real XCB/lavapipe and Wayland/Weston paths, repeats
the retained-owner lifecycle, compares cold and hot mapped readbacks with the
B3B1 oracle byte-for-byte, runs ASan/UBSan/leak detection, compiles the Win32
Vulkan SDK path, and records three hosted benchmark distributions.

The fixed fixture remains:

- surface: 640 × 360;
- commands: 68;
- fill instances: 65;
- glyph instances: 240;
- persistent atlas pages: 3;
- resident atlas bytes: 49,152;
- output/readback bytes: 921,600.

## Next slice

Z2F-8B3B2C will execute the same packet through a retained Metal device and
command queue using a native Metal compute pipeline and exact readback
comparison.
