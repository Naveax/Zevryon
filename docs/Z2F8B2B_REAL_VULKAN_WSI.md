# Z2F-8B2B: Real Vulkan Window-System Integration

## Scope

Z2F-8B2B binds the bounded Z2F-8B1 window-swapchain state machine to real
Vulkan WSI objects. The implementation supports Win32, XCB and Wayland surface
creation, a real `VkSwapchainKHR`, acquired back buffers, graphics and present
queues, binary acquire/render semaphores, per-frame command pools and native
completion fences.

This stage clears the acquired image using the certified command checksum. It
does not yet install the final glyph/fill shader pipeline or upload the atlas
pixels; those are the next compositor stage.

## Single-device ownership

Vulkan handles do not expose COM-style `AddRef`. Exporting raw `VkDevice` and
allowing the Z2F-8A owner to shut down would leave the presenter with destroyed
handles. The WSI owner therefore exports a private ref-counted context lease.

The lease owns exactly one:

- `VkInstance` created with `VK_KHR_surface` and the selected platform surface
  extension;
- `VkSurfaceKHR` for the caller-provided Win32, XCB or Wayland window;
- physical device;
- `VkDevice` with `VK_KHR_swapchain`;
- graphics queue and presentation queue.

`NativeGpuSdkContextHandle` continues to carry the stable public ABI. A private
context flag marks the opaque lease. `VulkanNativeWindowSwapchainApi` retains
that lease during configuration. The WSI owner may then close; instance,
surface, device and queues remain alive until the presenter releases the final
lease. The presenter never enumerates another adapter and never creates a
second device.

## Queue-family selection

Device creation requires both graphics and presentation support. A single
queue family is preferred. When graphics and present families differ, both
queues are created on the same `VkDevice` and swapchain images use concurrent
sharing across the two family indices.

## Swapchain lifecycle

Configuration queries surface capabilities, formats and present modes and then
creates a bounded image ring. Supported modes map as follows:

- FIFO -> `VK_PRESENT_MODE_FIFO_KHR`;
- Mailbox -> `VK_PRESENT_MODE_MAILBOX_KHR`;
- Immediate -> `VK_PRESENT_MODE_IMMEDIATE_KHR`.

Every in-flight frame owns one command pool, primary command buffer, acquire
semaphore, render-finished semaphore and fence. The configured frame and image
counts remain bounded by the Z2F-8B1 limits.

Acquisition maps Vulkan status directly to the stable API:

- `VK_SUCCESS` -> Acquired;
- `VK_SUBOPTIMAL_KHR` -> Suboptimal;
- `VK_TIMEOUT` or `VK_NOT_READY` -> NotReady;
- `VK_ERROR_OUT_OF_DATE_KHR` -> OutOfDate;
- `VK_ERROR_DEVICE_LOST` -> DeviceLost.

## Presentation

The command buffer transitions the back buffer to transfer-destination layout,
clears it using the command checksum and returns it to present layout. Queue
submission waits on the image-acquired semaphore and signals the render-finished
semaphore. `vkQueuePresentKHR` waits on that semaphore.

An acquired Vulkan image cannot be abandoned. Consequently a no-damage frame
still performs an empty native submission and a real present; it is reported as
Presented rather than falsely reported as SkippedNoDamage.

When `VK_KHR_incremental_present` is available and partial presentation is
enabled, bounded damage rectangles are projected through `VkPresentRegionsKHR`.

## Recreation and failure model

Resize or `VK_ERROR_OUT_OF_DATE_KHR` marks the swapchain out of date. Recreation
requires all acquired and in-flight frames to be drained, advances both surface
and swapchain generation, supplies the old swapchain to Vulkan and destroys the
old image ring only after the new one is ready.

The layer fails closed for:

- stale device, runtime, window, surface, swapchain or image generation;
- unavailable platform surface extension;
- absent graphics or present queue;
- unsupported present mode or transfer-destination image usage;
- surface, image, damage or in-flight budget overflow;
- unretired frame-ring backpressure;
- fence regression;
- out-of-date, suboptimal or lost-device results.

## Certification

The focused workflow contains:

- ten real XCB lifecycle repetitions under Xvfb and Mesa lavapipe;
- three 256-frame real-present distributions;
- ten real Wayland lifecycle repetitions under headless Weston;
- real XCB execution under ASan, UBSan and leak detection;
- Win32 Vulkan SDK compilation for surface, swapchain and benchmark paths;
- Z2F-8A, Z2F-8B1 and repository-wide regression workflows.

The benchmark fixes the 640 x 360, three-image topology at 2,764,800 logical
surface bytes and 921,600 peak in-flight bytes. Its deterministic checksum is
`12529228277956645091`.

## Explicit boundary

Z2F-8B2B presents real Vulkan back buffers to real operating-system surfaces.
It does not yet create graphics pipelines, shaders, descriptor sets, atlas
textures, vertex buffers, selection geometry or caret geometry. Those belong
to Z2F-8B3, which will replace the checksum clear with the final compositor
shader and atlas-sampling pipeline.
