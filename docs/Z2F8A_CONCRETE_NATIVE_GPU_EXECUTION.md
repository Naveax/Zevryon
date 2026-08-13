# Z2F-8A: Concrete Native GPU SDK Execution and Offscreen Surface Bridge

## Current support status

Z2F-8A currently binds the certified Z2F-7 `NativePlatformSubmission` contract
to Vulkan and Direct3D 12 SDK objects. Metal/macOS support has been removed.
The historical `NativeGpuApiKind::Metal` value and legacy Metal factory symbol
remain only for ABI/source compatibility and are fail-closed: the probe is
`Unavailable`, capabilities and default limits are zero, and initialization
returns `UnsupportedBackend`.

## Scope

The supported execution paths create a native device and queue, allocate a
bounded ring of offscreen renderable images, record one real GPU command
buffer/list, submit it to the native queue, wait on a native completion
primitive and publish a generation-safe receipt.

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
`NativePlatformDriver` interface. Supported Vulkan/D3D12 adapter wrappers can
therefore consume the concrete SDK driver without changing the upper compositor
pipeline. Retained Metal identities do not create a usable driver.

## Build-graph isolation

Concrete device execution depends on the stable Z2F-7 submission ABI, not on
font discovery or shaping. When the complete text-to-adapter graph exists,
Z2F-8A links to `zevryon-native-platform-adapters`. When that graph is absent,
a compact standalone translation unit provides only the
`NativePlatformSubmission` allocator and reset lifecycle required by the SDK
boundary.

This dual construction has the same public records and execution behavior. It
allows Direct3D 12 device, command-list and WARP certification on a default
Windows build without installing HarfBuzz, while Linux certification exercises
the complete text-to-adapter chain. macOS is not a supported certification or
build target.

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

## Removed Metal path

The former Objective-C++ Metal implementation and macOS build graph were
removed. No `MTLDevice`, `MTLCommandQueue`, Metal texture, command-buffer or
encoder implementation is shipped. Constructing the retained reference/legacy
Metal API identity cannot create a device or surface and fails closed before
normal configuration validation.

## Failure and generation rules

The execution layer rejects:

- unsupported backend identities, including Metal;
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

The current cross-platform reference SDK certification covers Vulkan and
Direct3D 12 successful lifecycles. Metal is covered only as an explicit
unavailable/unsupported negative contract. The independent supported-backend
oracle contains 6,144 cases rather than the historical 9,216 three-backend
cases.

Separate real native smoke certification remains hardware/runtime-specific and
is not implied by ordinary headless CI. Normal Windows/Linux CI compiles the
native targets and runs the hardware-independent suite; true GPU authority is a
separate certification boundary.

## Explicit boundary

Z2F-8A proves supported native device, resource, command-buffer and queue
execution for Vulkan and Direct3D 12. It does not:

- create Win32, XCB or Wayland window swapchains;
- provide Cocoa/Metal window or GPU execution support;
- acquire real window-system drawable/back-buffer objects;
- compile glyph/fill shaders or graphics pipelines;
- upload atlas pixel payloads through a native staging ring;
- draw final selection, glyph and caret geometry;
- issue partial-present rectangles to a window compositor;
- survive device loss by rebuilding the full renderer graph.

Those supported-platform operations belong to Z2F-8B, which connects the
certified execution lifecycle to visible operating-system surfaces and final
compositor shaders.
