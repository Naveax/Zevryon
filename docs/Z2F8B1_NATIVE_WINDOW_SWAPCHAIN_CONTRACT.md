# Z2F-8B1: Native Window Swapchain Contract and Generation-Safe Lifecycle

## Current support status

Z2F-8B1's current native window support targets Windows and Linux. The generic
ABI still contains historical Metal and `CocoaLayer` identity values so stable
record numbering and serialized contracts do not need to be renumbered, but
those values are unsupported. Metal/Cocoa default swapchain capabilities and
limits are zero and configuration fails closed. No Cocoa, QuartzCore,
`CAMetalLayer` or Metal presenter implementation is shipped.

## Scope

Z2F-8B1 freezes the window-system swapchain contract that sits between the
certified Z2F-8A native GPU device/queue execution layer and the visible
compositor shader pipeline.

The stage deliberately does not create a second GPU device. Instead it defines
an opaque `NativeGpuSdkContextHandle` that carries the already-certified native
instance/factory, physical-device/adapter, device, graphics queue and present
queue identities together with device/runtime generations. Supported platform
WSI implementations consume that handoff so the renderer retains one device
graph.

## Stable records

The public header contains no Vulkan, Direct3D, XCB, Wayland or Win32 SDK types.
Historical Metal/Cocoa enum identities are plain stable C++ values, not SDK
objects and not a support declaration.

The fixed records are:

- `NativeGpuSdkContextHandle` — 72 bytes;
- `NativeWindowSwapchainCapabilities` — 40 bytes;
- `NativeWindowSwapchainLimits` — 40 bytes;
- `NativeWindowSwapchainConfig` — 208 bytes;
- `NativeWindowSwapchainImage` — 96 bytes;
- `NativeWindowPresentReceipt` — 152 bytes.

The context record preserves graphics and present queue identities separately.
A backend may advertise that both queues are shared, but callers cannot assume
that topology.

## State machine

The certified lifecycle is:

1. configure one supported window surface and bounded image ring;
2. acquire one generation-safe image lease;
3. present that exact lease with bounded damage;
4. retain it until its signal fence is retired;
5. stop acquisition when the frame-in-flight limit is reached;
6. mark the surface out of date on resize;
7. drain acquired and in-flight images;
8. recreate with strictly newer surface and swapchain generations;
9. reject all tokens from the old generation.

The state machine distinguishes acquired, not-ready/backpressure, suboptimal,
out-of-date, occluded and device-lost states. No status is converted into silent
success.

## Bounded ownership

The contract fixes independent limits for:

- swapchain image count;
- frames in flight;
- damage rectangle count;
- surface width and height;
- retained surface bytes;
- bytes owned by in-flight images.

Acquisition is denied before the configured frame-in-flight envelope can be
exceeded. Surface byte accounting is checked before publication. Width, height,
bytes-per-pixel, image count and in-flight multiplication are overflow checked.

## Resize and recreation

A resize request must preserve the surface identifier, advance the surface
generation and provide a non-zero bounded extent. After a resize request,
acquisition returns `OutOfDate`. Recreation is rejected until all acquired and
in-flight images have been released or retired.

Successful recreation must use the pending surface descriptor, the same
device/runtime generations, the same window generation and a strictly newer
swapchain generation.

This models supported Vulkan swapchain replacement and DXGI
`ResizeBuffers`/swapchain recreation without exposing platform SDK types in the
public ABI. Historical `CocoaLayer`/Metal tokens do not participate in current
recreation support.

## Present validation

A present request is accepted only when the image token matches:

- device generation;
- runtime generation;
- surface identifier and generation;
- swapchain generation;
- image generation;
- native resource identifier;
- current acquire serial.

Damage rectangles must be non-empty, non-negative and entirely inside the
surface. Empty damage produces `SkippedNoDamage` unless a full redraw is
explicitly requested.

Tearing is accepted only when both the backend and configuration allow it and
the selected present mode is immediate.

## Certification

The reference implementation certifies the platform-independent lifecycle for
supported backend identities before native WSI bindings are exercised.

Coverage includes:

- configure/acquire/present/retire;
- suboptimal acquisition and presentation;
- empty-damage skip;
- stale device/runtime/surface/swapchain/image generations;
- damage bounds;
- frame backpressure;
- occlusion;
- device loss;
- resize and recreation;
- fence regression;
- unsupported present modes, backend identities and window systems;
- resource and arithmetic limits.

The independent supported-backend oracle spans Vulkan and Direct3D 12 contract
topologies and contains 6,144 cases. Metal/Cocoa is covered as an explicit
negative contract, not as a successful third topology.

The deterministic benchmark uses:

- a 16,384-line source-document boundary;
- an 80-line projected viewport;
- two supported backend contracts;
- three swapchain images per supported backend;
- two frames in flight;
- four damage rectangles;
- 80 compositor commands per presentation;
- a 1,920 × 1,080 surface.

## Explicit boundary

Z2F-8B1 freezes ownership, generation and recreation semantics. Current native
implementations may export real Vulkan/D3D12 SDK context handles and bind the
supported Windows/Linux WSI paths. This contract does not provide or promise
Cocoa, `CAMetalDrawable`, `CAMetalLayer`, Objective-C++ or Metal presentation.
Those names may appear only where historical ABI identities are intentionally
retained.
