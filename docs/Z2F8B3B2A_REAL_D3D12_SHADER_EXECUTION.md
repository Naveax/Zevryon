# Z2F-8B3B2A — Real Direct3D 12 Integer Shader Execution

## Scope

This slice binds the backend-independent Z2F-8B3B1 draw packet to a real
Direct3D 12 compute pipeline. It removes the Z2F-8B3A full-frame CPU pixel
composition from the D3D12 rendering path while retaining the CPU executor only
as a pixel oracle.

The executor retains the existing Z2F-8A `ID3D12Device` and
`ID3D12CommandQueue` through COM references. It does not enumerate another
adapter, create another device, or create another graphics queue.

## Integer shader model

A runtime-compiled CS 5.1 shader executes one thread per output pixel. Each
thread consumes the exact packet command order and applies:

- selection fills;
- Alpha8 glyph coverage;
- LCD RGB channel coverage;
- premultiplied BGRA color glyphs;
- caret fills;
- integer nearest-neighbour atlas sampling;
- exact integer source-over composition.

The shader reproduces the reference executor's `mul_u8` and blend equations.
The output is an `R32_UINT` UAV whose little-endian bytes are canonical BGRA8.
This avoids backend-dependent floating-point blending and permits byte-for-byte
readback comparison.

## Persistent resources

Resident B3B1 atlas pages are uploaded into one persistent
`Texture2DArray<R32_UINT>`. Page index and generation are mapped to stable array
slices. The texture is rebuilt only when the resident page signature changes;
hot packets reuse the same native atlas allocation.

Command, fill and glyph records use bounded structured upload buffers. The
output UAV and readback buffer are reused while surface dimensions remain
unchanged.

## Failure model

The executor rejects mutated packet checksums, invalid scissors, missing page
generations, mixed generations for one page, packet-budget overflow, atlas
budget overflow, stale native context and GPU failures. Readback publication is
transactional and occurs only after command completion.

## Certification

The focused Windows workflow executes the 68-command fixture with 65 fill
instances, 240 glyph instances and three persistent atlas pages. Cold and hot
GPU readbacks must exactly match the Z2F-8B3B1 reference readback. The owner SDK
is destroyed before hot execution to verify retained native-context ownership.

## Next slices

Z2F-8B3B2B will execute the same packet through Vulkan descriptor sets and a
real compute/graphics pipeline. Z2F-8B3B2C will add the corresponding Metal
pipeline and readback equivalence.
