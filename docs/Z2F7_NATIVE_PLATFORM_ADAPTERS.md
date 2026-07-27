# Z2F-7: Native Vulkan, Metal and Direct3D 12 Adapter Contract

## Scope

Z2F-7 consumes the certified Z2F-6 `NativeCommandBuffer`, Z2F-4 frame payload
and draw-instance spans. It translates the bounded backend-neutral command
stream into one compact native-platform submission contract for Vulkan, Metal
and Direct3D 12 adapters.

The public contract deliberately does not expose SDK headers or operating-
system handles. Platform translation records are stable C++ data structures;
platform-specific translation units may bind them to Vulkan, Metal or D3D12
without changing the upper compositor pipeline.

## Adapter boundary

`NativePlatformGpuCommandApi` implements the existing Z2F-6
`NativeGpuCommandApi`. Three concrete wrappers select the platform contract:

- `VulkanNativeGpuCommandApi`;
- `MetalNativeGpuCommandApi`;
- `Direct3D12NativeGpuCommandApi`.

Each wrapper uses a `NativePlatformDriver`. Production drivers will own the
actual Vulkan device and swapchain, Metal device/queue/layer, or D3D12 device,
queue and DXGI swapchain. `ReferenceNativePlatformDriver` is deterministic and
platform-independent; it certifies adapter topology, generations, fences and
failure handling in hosted CI.

## Command translation

One Z2F-6 render pass is translated into the following bounded sequence:

1. begin native command buffer/list;
2. transition acquired surface image from present to render-target state;
3. begin render pass/encoder;
4. apply every certified damage scissor;
5. encode intersecting fill commands;
6. bind one descriptor per unique atlas page and encode glyph batches;
7. end render pass/encoder;
8. transition the surface image back to present state;
9. close and submit the command buffer/list;
10. present the acquired image.

Vulkan adapters interpret transition records as image-layout and access/stage
barriers. Metal adapters interpret the same boundary as command-encoder and
load/store state. D3D12 adapters interpret it as resource-state barriers and
DXGI present parameters.

## Bounded resources

The adapter config has explicit hard limits for:

- translated command records;
- barrier records;
- descriptor bindings;
- swapchain images;
- frames in flight;
- driver staging bytes.

`encode_submit_present` uses a fixed 256 KiB scratch arena with no upstream
allocator. Budget exhaustion fails closed. Descriptor bindings are deduplicated
by atlas generation, page generation, page index and raster format. Glyph
payloads and draw instances remain owned by earlier Z2 stages.

## Identity and synchronization

Every acquired platform image carries:

- device generation;
- surface and surface generation;
- image generation and index;
- driver generation;
- native resource ID;
- current resource state.

Compilation rejects stale images, stale command buffers, generation mismatch,
invalid draw spans, page-identity mismatch, missing upload completion, and
capacity overflow. Reference drivers publish strictly monotone signal fences.

## Capabilities

Capabilities are explicit and testable:

- timeline fences;
- partial present;
- mailbox and immediate present modes;
- tearing;
- explicit resource barriers;
- unified memory.

Vulkan and D3D12 expose explicit barriers and tearing-capable immediate present
in the reference capability model. Metal exposes unified memory and does not
advertise tearing.

## Compact records

| Record | Bytes |
|---|---:|
| `NativePlatformCapabilities` | 32 |
| `NativePlatformAdapterConfig` | 48 |
| `NativePlatformCommandRecord` | 32 |
| `NativePlatformBarrierRecord` | 32 |
| `NativePlatformDescriptorBinding` | 32 |
| `NativePlatformSwapchainImage` | 64 |

## Certification fixture

The fixed benchmark models the existing Z2F viewport contract:

- source document: 16,384 lines;
- projected viewport: 80 lines;
- Z2F-4 frame commands: 68;
- Z2F-6 native commands: 71;
- draw instances: 310;
- three native backends;
- 80 translated commands per backend;
- two surface-state barriers per backend;
- three deduplicated atlas descriptors per backend;
- 2,720 logical retained bytes per backend;
- 8,160 logical retained bytes across all adapters;
- fixed adapter scratch hard limit: 262,144 bytes;
- deterministic aggregate checksum: `943884624265623740`.

The independent oracle covers 9,216 backend, damage and command-topology cases.

## Failure model

The layer fails closed for unsupported capabilities, invalid configuration,
stale surface or image generations, stale command buffers, invalid fill/glyph
payload indices, invalid draw-instance spans, page identity mismatch, upload
fence readiness failure, record-capacity exhaustion, scratch-budget exhaustion,
fence overflow, surface out-of-date and device loss.

## Explicit boundary

Z2F-7 establishes production adapter classes and the exact native translation
contract. This stage does not yet include Vulkan SDK calls, Objective-C++ Metal
calls, D3D12/DXGI calls, shader binaries, pipeline creation, OS window handles,
platform loader discovery, or actual pixel presentation. Those implementations
can now be added behind the certified driver ABI without altering Z2F-6 or the
higher text pipeline.
