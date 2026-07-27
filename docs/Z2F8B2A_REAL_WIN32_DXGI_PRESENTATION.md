# Z2F-8B2A — Real Win32 and DXGI Window Presentation

## Scope

Z2F-8B2A attaches Zevryon's native presentation contract to a real Win32
window and a real DXGI flip-model swapchain.

The implementation consumes the Direct3D 12 device graph created by Z2F-8A.
It does not enumerate another adapter, create another `ID3D12Device`, or create
a second graphics queue.

This is the Windows production slice of Z2F-8B2. Vulkan XCB/Wayland WSI and
Metal `CAMetalLayer` presentation are separate slices because they require
backend-specific initialization changes that are not safely representable by
the current headless Vulkan and Metal execution setup.

## Single-device invariant

The ownership chain is:

```text
Z2F-8A Direct3D 12 execution backend
  IDXGIFactory6
  IDXGIAdapter1
  ID3D12Device
  ID3D12CommandQueue
          |
          | NativeGpuSdkApi::export_context()
          v
Z2F-8B2A Direct3D 12 window presenter
  AddRef retained factory/device/queue
  HWND-backed IDXGISwapChain3
  real DXGI back buffers
```

`NativeGpuSdkContextHandle` transports opaque identities and generation
numbers. Platform SDK types do not appear in public headers.

The Direct3D 12 presenter retains the exported COM objects with `AddRef`. The
original Z2F-8A API object may therefore release its own references without
creating a second device or invalidating the presenter's retained context.

## Context export contract

`NativeGpuSdkApi::export_context()` has a safe unsupported default.

Only a backend that can provide a valid native device graph overrides it. In
this slice, the Direct3D 12 backend exports:

- DXGI factory identity;
- selected adapter identity;
- D3D12 device identity;
- graphics queue identity;
- present queue identity;
- device generation;
- runtime generation;
- shared graphics/present queue flag;
- software-device flag when WARP is selected.

Calling export before device initialization fails with
`RuntimeUnavailable`. Unsupported backends fail with `UnsupportedBackend` and
zero the output record.

## Real window surface

The presenter requires:

- `NativeGpuApiKind::Direct3D12`;
- `NativeWindowSystem::Win32`;
- a live HWND;
- a complete exported native context;
- nonzero device, runtime, window, surface, and swapchain generations;
- bounded image, damage, extent, surface-byte, and in-flight-byte limits.

The swapchain uses:

- `DXGI_SWAP_EFFECT_FLIP_DISCARD`;
- `DXGI_USAGE_RENDER_TARGET_OUTPUT`;
- two or more real back buffers;
- explicit BGRA8 or RGBA8 format mapping;
- optional `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` when supported.

Mailbox mode is rejected because DXGI flip-model swapchains do not expose a
mailbox present mode equivalent through this contract.

## Acquire and image leases

`acquire()` reads the current real back-buffer index from
`IDXGISwapChain3::GetCurrentBackBufferIndex()`.

Each acquired image is protected by:

- device generation;
- runtime generation;
- surface ID and generation;
- swapchain generation;
- image generation;
- native resource identity;
- acquire serial;
- acquired and in-flight state.

A stale image from an older swapchain or surface generation cannot be
presented after recreation.

The configured frames-in-flight limit applies before a back buffer is handed
to the caller. When the bounded ownership count is full, acquire returns
`NotReady` rather than allocating additional state.

## Command recording and presentation

Each back buffer owns a bounded command allocator and graphics command list.
Presentation records:

1. transition from `D3D12_RESOURCE_STATE_PRESENT` to
   `D3D12_RESOURCE_STATE_RENDER_TARGET`;
2. deterministic render-target clear derived from the command checksum;
3. transition back to `D3D12_RESOURCE_STATE_PRESENT`;
4. execution on the exported Z2F-8A direct queue;
5. `IDXGISwapChain::Present1`;
6. monotonic fence signal.

This proves that the acquired resource is a real operating-system swapchain
back buffer rather than an offscreen texture ring.

## Damage rectangles

When partial presentation is enabled, bounded Zevryon damage rectangles are
validated against the current surface and converted to Win32 `RECT` records
for `DXGI_PRESENT_PARAMETERS`.

A frame with no damage and without the full-redraw flag releases the acquired
lease without submitting GPU work or calling present.

## Present modes

### FIFO

FIFO uses sync interval 1 and does not allow tearing.

### Immediate

Immediate uses sync interval 0. Tearing is accepted only when:

- the swapchain was configured with tearing enabled;
- DXGI reports `DXGI_FEATURE_PRESENT_ALLOW_TEARING`;
- the request explicitly allows tearing.

## Occlusion and device loss

`DXGI_STATUS_OCCLUDED` maps to `NativeWindowPresentStatus::Occluded`.
Subsequent acquire operations use `DXGI_PRESENT_TEST` to determine when the
window becomes presentable again.

`DXGI_ERROR_DEVICE_REMOVED` and `DXGI_ERROR_DEVICE_RESET` map to `DeviceLost`.
The presenter does not silently create a replacement device.

## Resize and recreation

A resize request must advance the surface generation and remain inside all
configured limits. It marks the active swapchain out of date.

Recreation requires:

- no acquired images;
- no in-flight images;
- the same device and runtime generations;
- the same window generation;
- a newer swapchain generation;
- the exact pending surface descriptor.

The real DXGI swapchain is resized with `ResizeBuffers`. All old back-buffer
resources, RTVs, command allocators, command lists, image generations, and
leases are invalidated.

## Fence retirement

Every submitted present signals a monotonically increasing D3D12 fence value.

`retire_completed()` rejects regression and values beyond the submitted
timeline, waits for native completion, and releases all image ownership whose
fence is complete. Surface and in-flight byte accounting are updated exactly.

## Certified boundaries

The focused certification covers:

- pre-initialization context-export rejection;
- real hardware or WARP device initialization;
- complete context export;
- visible Win32 window creation;
- real swapchain configuration;
- acquire, clear, `Present1`, and fence retirement;
- no-damage skip;
- bounded frames-in-flight backpressure;
- resize and `ResizeBuffers` recreation;
- stale old-image rejection;
- exact surface and in-flight byte accounting;
- deterministic record-size and generation contracts;
- repeated real-present latency distributions.

## Deliberate exclusions

Z2F-8B2A does not yet provide:

- Vulkan instance surface extensions;
- Vulkan presentation queue-family selection;
- `VkSurfaceKHR` or `VkSwapchainKHR`;
- XCB or Wayland window attachment;
- `CAMetalLayer` attachment;
- `CAMetalDrawable` acquisition;
- final glyph shaders or atlas sampling.

Those capabilities must be added without weakening the single-device,
generation-safe ownership model established here.
