# Z2F-8A: Concrete Native GPU SDK Execution and Offscreen Surface Bridge

## Scope

Z2F-8A binds the certified Z2F-7 `NativePlatformSubmission` contract to real
Vulkan, Metal and Direct3D 12 SDK objects. It creates a native device and queue,
allocates a bounded ring of offscreen renderable images, records one real GPU
command buffer/list, submits it to the native queue, waits on a native
completion primitive and publishes a generation-safe receipt.

The stage remains bounded by configured image count, frames in flight, command
count, descriptor count, staging bytes and device-local bytes. Total document
size does not participate in execution or retained GPU state.

This slice intentionally uses an offscreen surface bridge. Operating-system
window swapchains and the final glyph/fill shader pipeline remain Z2F-8B.

## Public ABI

Platform SDK types remain private to platform translation units. The public
header exposes only stable C++ records:

- `NativeWindowSurfaceHandle` — opaque window/layer handles and generation;
- `NativeGpuSdkLimits` — image, frame, allocator, descriptor and byte limits;
- `NativeGpuSdkConfig` — API selection, generations and execution policy;
- `NativeGpuSdkProbe` — compile/runtime availability and device identity;
- `NativeGpuSdkSubmissionReceipt` — command topology, fences and checksum;
- `NativeGpuSdkSnapshot` — bounded resource and lifecycle accounting.

`NativeGpuSdkPlatformDriver` implements the existing Z2F-7
`NativePlatformDriver` interface. Existing Vulkan/Metal/D3D12 adapter wrappers
therefore consume the concrete SDK driver without changing the upper
compositor pipeline.

## Build-graph isolation

Concrete device execution depends on the stable Z2F-7 submission ABI, not on
font discovery or shaping. When the complete text-to-adapter graph exists,
Z2F-8A links to `zevryon-native-platform-adapters`. When that graph is absent,
a compact standalone translation unit provides only the
`NativePlatformSubmission` allocator and reset lifecycle required by the SDK
boundary.

This dual construction has the same public records and execution behavior. It
allows Direct3D 12 device, command-list and WARP certification on a default
Windows build without installing HarfBuzz, while Linux and macOS certification
continue to exercise the complete text-to-adapter chain. The standalone path
must not implement platform-command compilation or duplicate Z2F-7 adapter
logic.

## Vulkan execution

When Vulkan is available, the backend creates:

- `VkInstance`;
- one selected physical device;
- one graphics queue family;
- `VkDevice` and graphics queue;
- transient/resettable command pool;
- native submission fence;
- a bounded ring of device-local color images.

A submission allocates one primary command buffer, transitions the acquired
image into a writable layout, executes a deterministic GPU clear representing
the certified platform submission envelope, submits to the graphics queue and
waits for native fence completion. CPU Vulkan devices such as Mesa lavapipe are
accepted only when the configuration permits software devices and are reported
with an explicit capability flag.

## Metal execution

The Metal translation unit creates:

- `MTLDevice`;
- `MTLCommandQueue`;
- a bounded ring of private render-target textures;
- `MTLCommandBuffer` and `MTLRenderCommandEncoder` per submission.

The render encoder performs a deterministic clear into the acquired texture,
commits the real command buffer and waits for completion. Metal runtime errors
are converted to explicit SDK errors; no partially submitted receipt is
published.

## Direct3D 12 execution

The Windows backend creates:

- DXGI factory and selected adapter;
- hardware D3D12 device, or WARP when policy permits;
- direct command queue;
- command allocator and graphics command list;
- fence and event;
- bounded RTV descriptor heap;
- bounded device-local render-target resources.

Each submission resets the allocator/list, transitions the acquired resource to
`RENDER_TARGET`, clears it, transitions it back to the offscreen-present state,
executes the command list, signals the native fence and waits for completion.

## Failure and generation rules

The execution layer rejects:

- zero or mismatched device/runtime generations;
- non-headless window systems in Z2F-8A;
- stale surface or acquired-image generations;
- wrong API kind;
- image, frame, allocator, descriptor or command limit overflow;
- staging or device-local byte budget overflow;
- unavailable SDK runtime;
- native device/queue/resource creation failure;
- native command encoding or queue submission failure;
- fence regression or completion beyond submitted work;
- native device-loss results.

All configuration and resource publication is failure-atomic. Native resources
created before an error are destroyed before the method returns.

## Deterministic certification

The cross-platform reference SDK implementation runs the same bounded lifecycle
for Vulkan, Metal and Direct3D 12. A 9,216-case independent oracle covers API,
device, runtime and surface generations; command/descriptor limits;
acquire/present statuses; device loss; and fence retirement.

The deterministic benchmark models the certified 16,384-line document and
80-line viewport boundary using three backends, 80 platform commands, two
barriers and three descriptors per submission. It fixes record sizes, image
memory, command topology and per-backend checksums independently of host GPU
hardware.

Separate platform smoke tests exercise real native queue submission plus fence
completion:

- Mesa Vulkan/lavapipe on Linux hosted CI with the complete adapter graph;
- Metal on macOS hosted CI with the complete adapter graph;
- D3D12 hardware or WARP on Windows hosted CI through the standalone stable-ABI
  build path.

## Explicit boundary

Z2F-8A proves real native device, resource, command-buffer and queue execution.
It does not yet:

- create Win32, Cocoa, XCB or Wayland window swapchains;
- acquire real window-system drawable/back-buffer objects;
- compile glyph/fill shaders or graphics pipelines;
- upload atlas pixel payloads through a native staging ring;
- draw final selection, glyph and caret geometry;
- issue partial-present rectangles to a window compositor;
- survive device loss by rebuilding the full renderer graph.

Those operations belong to Z2F-8B, which will connect the certified execution
lifecycle to visible operating-system surfaces and final compositor shaders.
