# Z2F-8B3B1 — Bounded Shader Draw Packet and Persistent Atlas Contract

## Scope

Z2F-8B3B1 removes the full-surface CPU pixel buffer from the renderer-facing
contract. It compiles viewport-bounded selection, glyph and caret primitives into
one exact-size shader packet that can be consumed by D3D12, Vulkan and Metal.

The CPU composer from Z2F-8B3A remains only as a reference oracle. Production
backends receive structured instance, scissor, draw-command and atlas-upload
records instead of a 640×360×4-byte frame copy.

## Packet layout

The packet contains:

- a fixed header with frame, atlas and surface generations;
- deduplicated integer scissors;
- premultiplied fill instances for selections and carets;
- glyph quad instances with atlas page and generation references;
- draw commands in strict selection → glyph → caret order;
- exact cold atlas uploads and immutable payload bytes.

All public records are fixed-size and protected with compile-time assertions.
Packet publication is transactional: invalid ordering, geometry, atlas ranges,
generation mismatches, checksums, byte limits or allocation failures leave the
previous certified output untouched.

## Persistent atlas residency

Cold Alpha8, LCD RGB8 and BGRA uploads are expanded into a canonical four-byte
resident page representation. Hot packets carry no upload payload and reuse the
same page generation. Residency is bounded by configured page and byte limits,
tracks the last used frame and supports deterministic frame-age eviction.

## Reference shader executor

A standalone executor interprets the exact packet ABI using shader-equivalent
nearest sampling, scissoring and premultiplied source-over blending. It is not the
production rendering path. It exists to certify packet compilation and provide a
pixel oracle for native backend readback tests.

## Certified topology

The benchmark fixture contains:

- 68 draw commands;
- 65 fill instances;
- 240 glyph instances;
- one deduplicated full-surface scissor;
- three atlas uploads;
- 32,768 cold payload bytes;
- 49,152 resident canonical atlas bytes.

The cold packet is 52,640 bytes. The hot packet is 19,704 bytes and contains no
full-surface pixel payload.

## Next slice

Z2F-8B3B2 will bind this packet directly to native shader pipelines:

- D3D12 root signature, PSOs and persistent SRV atlas textures;
- Vulkan descriptor sets, graphics pipelines and image-layout-safe uploads;
- Metal render pipeline states, argument bindings and persistent textures;
- GPU readback comparison against the reference executor.
