# libs/gfx — CLAUDE.md

Guidance for Claude Code when working in this library. `libs/gfx` is the
**Vulkan device layer** promoted out of `ws::ui` (stage 3b): the loader,
the instance, the surface (via callback), physical-device/queue selection,
the device, and the VMA allocator. `ws::ui` is the only consumer — apps
never include this lib.

## The one rule: no Vulkan vocabulary under include/

The public header (`ws/gfx/gpu.hpp`) carries zero Vulkan types. Handles
cross the lib boundary as `void*`, each getter commented with the type it
actually is (`// VkDevice`); the consumer (`ui`'s `src/app/app.cpp`, which
includes volk.h) is the single unwrap point. `volk`, `vma` and
`vulkan-headers` are all PRIVATE on the link line — if a Vulkan type or
entry point ever needs to appear in a signature, it goes through `void*`
or a plain `uint32_t` (formats), never the spelled type.

`gfx` also must not include platform or GLFW code: the surface arrives
through `SurfaceSource` — `{ instance_extensions (+ count),
create_surface(instance, &surface_out, user), user }` — wired in ui's
`app.cpp`, whose `ui_gfx_create_surface` adapts the `WindowHost` seam.
The extension POINTER array is copied into the lib at `init()`; the
caller's array may be a temporary, but the strings it points to must
outlive the `Gpu`.

## Staged lifecycle (the Android surface-loss contract)

- `init(source, options)` — the **context stage**: volk + instance
  (+ validation when `options.enable_validation`). Once per process.
- `attach_surface()` — the **window stage**: creates the surface via the
  stored callback; the FIRST attach also runs the **device stage**
  (selection, device, queue, allocator), which then survives every
  detach. A failure rolls back the surface it created.
- `detach_surface()` — surface only; device stage stays up.
- `shutdown()` — guarded, idempotent full teardown (surface →
  allocator/device → messenger/instance → `volkFinalize` LAST).

Order misuse (attach before init, double init) is a returned `Status`,
never a crash or an assert.

## Error handling

`core::Status` everywhere; gfx logs NOTHING on failure — the message
rides the `Status` and the consumer logs it (`core::log_error`). gfx
speaks only two info lines directly (the GPU it picked, the texture tier)
and two warns (validation layer missing on an opt-in, messenger create
failed). Codes are the anon-namespace constants in `gpu.cpp` (order 1,
loader/instance 100s, surface 200s, device 300s); the category is
always `"gfx"`.

## The descriptor-indexing probe (M9, parameterized)

`DeviceOptions` carries the request: `want_bindless` +
`bindless_descriptor_limit` (ui passes the renderer batch's own
`kMaxBindlessTextures`, so the probe and the shader table can never
disagree). Every miss (1.0-only loader, no dialect, missing feature,
limits short of the requested table) degrades to the chunked fallback —
never a failure. The Vulkan-flavored queries stay in `gpu.cpp`; the
DECISION is the pure `decide_indexing()` in `src/gpu_choice.h`, tested
headless. The depth-format walk (`pick_depth_format`) is the same shape:
D32 → D24S8 → D16, chosen once at device selection, exposed as its
numeric value.

## Layout

```
libs/gfx/
  include/ws/gfx/gpu.hpp    # Gpu, SurfaceSource, DeviceOptions, DescriptorIndexing — PUBLIC, ws/core only
  src/gpu.cpp               # the bootstrap: instance, selection, device, allocator, teardown
  src/gpu_choice.h          # pure, Vulkan-free decision helpers (the testable half)
  src/vma_impl.cpp          # the workspace's single VMA_IMPLEMENTATION TU (moved from ui)
  tests/test_gfx.cpp        # headless: the pure helpers + the null-state/lifecycle contracts
```

## What is NOT here

The swapchain and frame sync stay in `ws::ui` until phase B; the
buffer/image/shader-module helpers (phase C) will grow here when the r2d
device half rewires onto them. Do not add API surface speculatively —
keep it only as large as ui actually uses.

## Tests

```
ctest --preset debug -R gfx
```

Headless by contract: the pure helpers, the source-shape guards and the
null-state/lifecycle-misuse paths run without a GPU, and no test fakes a
Vulkan device — a machine-dependent probe is a bug, not coverage.
