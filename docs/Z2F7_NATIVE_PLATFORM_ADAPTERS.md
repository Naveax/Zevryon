# Z2F-7: Native Vulkan and Direct3D 12 Adapter Contract

## Current support status

Zevryon's supported desktop GPU adapter targets are Vulkan and Direct3D 12.
Metal support was removed together with macOS support. The historical
`NativeGpuApiKind::Metal` identity and `MetalNativeGpuCommandApi` type name are
retained only for ABI/source compatibility; they do not advertise a usable
backend. Metal compile requests fail closed with `UnsupportedCapability`, the
reference Metal driver cannot configure a surface, and its default capabilities
are zero.

## Scope

Z2F-7 consumes the certified Z2F-6 `NativeCommandBuffer`, Z2F-4 frame payload
and draw-instance spans. It translates the bounded backend-neutral command
stream into one compact native-platform submission contract for the currently
supported Vulkan and Direct3D 12 adapters.

The public contract deliberately does not expose SDK headers or operating-
system handles. Platform translation records are stable C++ data structures;
platform-specific translation units may bind them to Vulkan or D3D12 without
changing the upper compositor pipeline.

## Adapter boundary

`NativePlatformGpuCommandApi` implements the existing Z2F-6
`NativeGpuCommandApi`. The supported concrete wrappers are:

- `VulkanNativeGpuCommandApi`;
- `Direct3D12NativeGpuCommandApi`.

`MetalNativeGpuCommandApi` remains only as a legacy compatibility type and is
fail-closed because no Metal platform driver is supported.

Each supported wrapper uses a `NativePlatformDriver`. Production drivers own
the actual Vulkan device and swapchain or D3D12 device, queue and DXGI
swapchain. `ReferenceNativePlatformDriver` is deterministic and
platform-independent; it certifies adapter topology, generations, fences and
failure handling in hosted CI for the supported backend identities.

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
barriers. D3D12 adapters interpret them as resource-state barriers and DXGI
present parameters. The retained Metal identity does not produce a submission.

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

Compilation rejects unsupported backend identities, stale images, stale command
buffers, generation mismatch, invalid draw spans, page-identity mismatch,
missing upload completion and capacity overflow. Reference drivers publish
strictly monotone signal fences.

## Capabilities

Capabilities are explicit and testable:

- timeline fences;
- partial present;
- mailbox and immediate present modes;
- tearing;
- explicit resource barriers;
- unified memory as a retained generic capability bit.

Vulkan and D3D12 expose their certified current capability models. Metal's
default capability record is entirely zero and cannot be configured.

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
- two supported native backend identities: Vulkan and Direct3D 12;
- 80 translated commands per backend;
- two surface-state barriers per backend;
- three deduplicated atlas descriptors per backend;
- 2,720 logical retained bytes per backend;
- 5,440 logical retained bytes across both adapters;
- fixed adapter scratch hard limit: 262,144 bytes.

The independent oracle covers 6,144 supported-backend, damage and
command-topology cases. Metal rejection is covered separately as a negative
contract rather than counted as a successful backend case.

## Failure model

The layer fails closed for unsupported backend identities or capabilities,
invalid configuration, stale surface or image generations, stale command
buffers, invalid fill/glyph payload indices, invalid draw-instance spans, page
identity mismatch, upload fence readiness failure, record-capacity exhaustion,
scratch-budget exhaustion, fence overflow, surface out-of-date and device loss.

## Historical note

The original Z2F-7 design included a Metal adapter topology and treated Vulkan,
Metal and Direct3D 12 as three reference backends. That history explains the
retained Metal enum/class identities in the stable C++ contract. It is not a
statement of current support. Current Zevryon desktop support is Windows and
Linux, with Vulkan and Direct3D 12 as the supported native GPU adapter paths.
