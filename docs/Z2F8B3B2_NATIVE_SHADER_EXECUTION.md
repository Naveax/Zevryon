# Z2F-8B3B2 — Native Shader Execution and GPU Readback Equivalence

## Scope

Z2F-8B3B2 consumes the certified Z2F-8B3B1 shader draw packet directly on
D3D12, Vulkan and Metal. It removes the production full-frame CPU composition
and upload path. Selection, glyph and caret primitives are evaluated by one
backend-equivalent integer compute shader and written to a native GPU output
buffer.

The Z2F-8B3A CPU composer remains the independent reference oracle. Every
platform backend reads the GPU output back during certification and compares
all bytes and the canonical BGRA checksum against that oracle.

## Fixed native ABI

| Record | Bytes |
|---|---:|
| `NativeShaderDrawRecord` | 48 |
| `NativeShaderFillRecord` | 32 |
| `NativeShaderGlyphRecord` | 64 |
| `NativeShaderScissorRecord` | 16 |
| `NativeShaderAtlasBinding` | 40 |
| `NativeShaderDispatchPlanHeader` | 96 |

The packet compiler preserves the certified selection → glyph → caret order,
deduplicated scissors, premultiplied BGRA colors, atlas page generations and
nearest-neighbor atlas coordinates. Packet, atlas and output byte totals are
checked with overflow-safe arithmetic before transactional publication.

## Exact integer compute compositor

HLSL, GLSL/SPIR-V and MSL implement identical per-pixel operations:

1. walk the ordered draw-command stream;
2. reject commands outside the active scissor;
3. apply premultiplied selection and caret fills;
4. sample persistent Alpha8, LCD RGB8 or BGRA atlas layers;
5. apply integer `/255` rounding using `(a*b+127)/255`;
6. execute premultiplied source-over blending;
7. publish one canonical packed BGRA8 output pixel.

Using integer compute rather than floating-point graphics blending makes the
backend readback byte-exact and avoids tolerance-based certification.

## Persistent atlas resources

Each backend allocates one persistent `R8G8B8A8_UINT` 2D-array texture. Atlas
pages are uploaded only when their generation differs from the resident native
layer. D3D12 and Vulkan publish generation metadata only after successful queue
submission; Metal `replaceRegion` publication is synchronous. Stale or missing
atlas generations fail closed.

## Native backend mapping

### Direct3D 12

- runtime-compiled HLSL compute shader;
- root constants and one SRV/UAV descriptor table;
- structured upload buffers for commands and instances;
- persistent `Texture2DArray<uint4>` atlas;
- default-heap UAV output and readback heap;
- native queue fence completion.

### Vulkan

- checked-in SPIR-V generated from the canonical GLSL source;
- descriptor set containing uniform/storage buffers and a combined atlas image;
- persistent `VK_FORMAT_R8G8B8A8_UINT` image array;
- host-visible structured buffers and output readback;
- command-pool, compute-pipeline and native fence submission on the retained
  graphics queue.

### Metal

- runtime-compiled MSL compute pipeline;
- shared structured buffers and output buffer;
- persistent `MTLTextureType2DArray` integer atlas;
- retained single-device `MTLCommandQueue` execution;
- synchronous command-buffer completion and exact readback.

## Failure model

The layer rejects invalid packet checksums, command counts, geometry, atlas
references, device/runtime generations, byte limits, allocation failures,
shader compilation errors, pipeline creation failures, queue submission
failures and readback mismatches. Previous certified plans and persistent atlas
generations are not changed by a failed publication.

## Explicit boundary

Z2F-8B3B2 produces a native GPU-rendered BGRA surface and certifies it through
readback. Connecting the output directly into each window back-buffer without a
readback/copy step is the next optimization boundary. The CPU composer remains
available only for oracle and fallback testing.
