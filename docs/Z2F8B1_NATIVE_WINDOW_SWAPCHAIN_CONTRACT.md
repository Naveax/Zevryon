# Z2F-8B1: Native Window Swapchain Contract and Generation-Safe Lifecycle

## Scope

Z2F-8B1 freezes the window-system swapchain contract that sits between the
certified Z2F-8A native GPU device/queue execution layer and the future visible
compositor shader pipeline.

The stage deliberately does not create a second GPU device. Instead it defines
an opaque `NativeGpuSdkContextHandle` that carries the already-certified native
instance/factory, physical-device/adapter, device, graphics queue and present
queue identities together with device/runtime generations. Platform WSI
implementations must consume that handoff so the renderer retains one device
graph.

## Stable records

The public header contains no Vulkan, Metal, Direct3D, XCB, Wayland, Win32,
AppKit or QuartzCore SDK types.

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

1. configure one window surface and bounded image ring;
2. acquire one generation-safe image lease;
3. present that exact lease with bounded damage;
4. retain it until its signal fence is retired;
5. stop acquisition when the frame-in-flight limit is reached;
6. mark the surface out of date on resize;
7. drain acquired and in-flight images;
8. recreate with strictly newer surface and swapchain generations;
9. reject all tokens from the old generation.

The state machine distinguishes:

- acquired;
- not ready due to backpressure;
- suboptimal;
- out of date;
- occluded;
- device lost.

No status is converted into silent success.

## Bounded ownership

The contract fixes independent limits for:

- swapchain image count;
- frames in flight;
- damage rectangle count;
- surface width and height;
- retained surface bytes;
- bytes owned by in-flight images.

Acquisition is denied before the configured frame-in-flight envelope can be
exceeded. This prevents a caller from owning an image that cannot legally be
presented.

Surface byte accounting is checked before publication. Width, height,
bytes-per-pixel, image count and in-flight multiplication are overflow checked.

## Resize and recreation

A resize request must:

- preserve the surface identifier;
- advance the surface generation;
- provide a non-zero bounded extent.

After a resize request, acquisition returns `OutOfDate`. Recreation is rejected
until all acquired and in-flight images have been released or retired.
Successful recreation must use:

- the pending surface descriptor;
- the same device/runtime generations;
- the same window generation;
- a strictly newer swapchain generation.

This models the requirements of Vulkan swapchain replacement, DXGI
`ResizeBuffers`/swapchain recreation and `CAMetalLayer` drawable reconfiguration
without exposing platform types in the public ABI.

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

The reference implementation certifies the platform-independent lifecycle
before native WSI bindings are added.

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
- unsupported present modes and window systems;
- resource and arithmetic limits.

An independent 9,216-case oracle spans Vulkan, Metal and Direct3D 12 contract
topologies.

The deterministic benchmark uses:

- a 16,384-line source-document boundary;
- an 80-line projected viewport;
- three backend contracts;
- three swapchain images per backend;
- two frames in flight;
- four damage rectangles;
- 80 compositor commands per presentation;
- a 1,920 × 1,080 surface.

## Explicit boundary

Z2F-8B1 freezes ownership, generation and recreation semantics. It does not yet:

- export real Z2F-8A SDK context handles from each backend;
- call `vkCreate*SurfaceKHR` or `vkCreateSwapchainKHR`;
- obtain `CAMetalDrawable` objects from a `CAMetalLayer`;
- create a DXGI swapchain for an `HWND`;
- compile fill, glyph, selection or caret shaders;
- upload atlas payloads through a native staging ring;
- present visible pixels to the operating-system compositor.

Those operations belong to Z2F-8B2. They must implement this contract and reuse
the existing Z2F-8A device/queue graph rather than creating parallel devices.
