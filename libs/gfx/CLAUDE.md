# libs/gfx — CLAUDE.md

Guidance for Claude Code when working in this library. `libs/gfx` is the
**Vulkan device layer** promoted out of `ws::ui` (stage 3b): the loader,
the instance, the surface (via callback), physical-device/queue selection,
the device, the VMA allocator, — since phase B — the swapchain and
frame sync, and — since phase C — the device-memory/object helpers
(host-write buffers, images, shader modules) ui's renderer draws
through. `ws::ui` is the only consumer — apps never include this lib.

## The one rule: no Vulkan vocabulary under include/

The public headers (`ws/gfx/gpu.hpp`, `ws/gfx/swapchain.hpp`) carry zero
Vulkan types. Handles
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
loader/instance 100s, surface 200s, device 300s), `swapchain.cpp`
(the shared order code, then swapchain-creation 400s) and `memory.cpp`
(the shared order code, then the create calls' 500s); the category is
always `"gfx"`.

The one exception — `Swapchain`'s frame flow (acquire, the render-pass
pair, submit, present): a Vulkan failure there leaves no state to roll
back to (semaphores in flight, a command buffer half recorded), so it
logs `core::error` and `std::abort()`s — the policy the shell documented
when it owned this code. `Swapchain`'s creation/teardown calls
(create/recreate/destroy) still answer `Status` like everything else.

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

## The swapchain and frame sync (phase B)

`ws::gfx::Swapchain` owns everything tied to the presentation extent:
the swapchain, its image views, the depth attachment (one per swapchain
— VMA owns the image, the view is ours), the color+depth render pass,
the framebuffers, and the whole frame sync — `kMaxFramesInFlight`
frame slots (a fence, a command pool+buffer and an acquire semaphore
each) plus one render-finished semaphore per image. `ws::ui` holds one
Swapchain beside its Gpu and is the only consumer; handles cross as
opaque `void*` exactly like Gpu's.

- **Lifecycle**: `create(Gpu&, fb_w, fb_h, options)` needs a Gpu with
  its device stage up and a surface attached (a dead Gpu is a `Status`,
  never a crash); a failed create cleans up after itself. `destroy()`
  is idempotent and keeps the Gpu + options, so create can run again on
  the re-attach path. `recreate(fb_w, fb_h)` is destroy+build with the
  full dead-Gpu guard. The Gpu must outlive the Swapchain.
- **Frame flow**: `acquire()` waits the slot fence BEFORE acquiring (the
  CPU throttle) and resets it only after the acquire succeeded — an
  out-of-date acquire bails having signaled nothing, the slot stays
  signaled and pristine. `VK_SUBOPTIMAL_KHR` is a success (the image IS
  acquired, its semaphore IS signaled): the frame must run and present,
  and `present()` folds the rebuild in once the frame has retired —
  never destroy a swapchain holding a live signaled semaphore.
  `submit()` waits the acquire semaphore at `COLOR_ATTACHMENT_OUTPUT`
  and signals the image's render-finished semaphore plus the slot
  fence; `present()` waits the image's semaphore.
  `begin_render_pass()` clears depth to 1.0 — the LEQUAL convention —
  through the color+depth pass, whose two explicit external subpass
  dependencies (color: `EXTERNAL`→`COLOR_ATTACHMENT_OUTPUT`, pairing
  with acquire's wait stage; depth: chained through the fragment-test
  stages) are what make the single shared depth image safe across
  frames in flight. `wait_frames()` waits every slot fence — the
  consumer's dev-only pipeline-rebake drain seam.
- **Choices**: the decision tables live in `src/swapchain_choice.h` as
  pure Vulkan-free helpers — surface format (the first preferred format
  offered in the required color space, else the surface's first entry),
  present mode (first preference supported, FIFO fallback; vsync-on
  never walks — FIFO is assigned directly, the one mode the spec
  guarantees), extent (the fixed current extent, else the framebuffer
  size clamped into the surface bounds; degenerate answers false),
  image count (min+1, clamped when the surface has a max), composite
  alpha (opaque, else the lowest supported bit). `swapchain.cpp` fills
  them from the surface queries and supplies the typed constants; the
  helpers only decide, so tests cover the tables headless.

## Device memory + object helpers (phase C)

`Gpu`'s memory/object methods (`src/memory.cpp`) are the ops ui's
renderer needs — sized to exactly its use, nothing speculative:

- **Buffers**: `create_buffer` is the one host-write profile ui has
  (sequential writes, persistently mapped — the arena + upload-staging
  shape); `flush_buffer` makes a whole buffer's writes visible (vma
  may hand back non-coherent memory, so a writer flushes after filling
  `mapped`); `destroy_buffer` takes the create's handle pair.
- **Images**: `create_image` is the one shape ui uploads — device-local
  2D sample targets (one mip, one layer, optimal tiling, starting
  undefined); the image VIEW stays the consumer's (a plain vk object,
  not memory).
- **Shader modules**: `create_shader_module` from SPIR-V words; the
  consumer casts the handle once and destroys after the pipelines are
  baked.
- **Guards**: each create clears its out record, validates the
  descriptor first (zero size/extent, empty words — detectable with no
  device), then requires the device stage; every failure is a `Status`.
  The destroys and the flush are null-tolerant no-ops, so failure-path
  teardown calls them uniformly — and a dead Gpu makes them no-ops too.

Since this phase `ws::ui` links no vma target and no `vk_mem_alloc.h`
include exists under `libs/ui`: r2d's arenas, staging buffers, textures
and shader modules all come through these methods, and the public
`allocator()` getter's only consumers are gfx's own TUs (this file's
creates, the swapchain's depth image).

## Layout

```
libs/gfx/
  include/ws/gfx/gpu.hpp        # Gpu, SurfaceSource, DeviceOptions, DescriptorIndexing — PUBLIC, ws/core only
  include/ws/gfx/swapchain.hpp  # Swapchain + Frame + SwapchainOptions, AcquireStatus/PresentStatus, kMaxFramesInFlight
  src/gpu.cpp                   # the bootstrap: instance, selection, device, allocator, teardown
  src/gpu_choice.h              # pure, Vulkan-free decision helpers (the testable half)
  src/swapchain.cpp             # the presentation layer: swapchain, depth, render pass, framebuffers, frame sync
  src/swapchain_choice.h        # pure, Vulkan-free swapchain decisions (the testable half)
  src/memory.cpp                # device memory + object helpers: buffers, images, shader modules
  src/vma_impl.cpp              # the workspace's single VMA_IMPLEMENTATION TU (moved from ui)
  tests/test_gfx.cpp            # headless: the pure helpers + the null-state/lifecycle contracts
```

## What is NOT here

Pipeline helpers stay in ui: r2d builds its pipelines straight against
the VkDevice (plain vk calls — no vma involved), and wrapping them here
would be surface ui does not need. Do not add API surface speculatively —
keep it only as large as ui actually uses.

## Tests

```
ctest --preset debug -R gfx
```

Headless by contract: the pure helpers (device choice + the swapchain
choice tables), the source-shape guards, Gpu's/Swapchain's
null-state/lifecycle-misuse paths and the phase-C memory/object guards
(descriptor validation, the device-stage Status, null-tolerant
destroys) run without a GPU, and no test fakes a Vulkan device — a
machine-dependent probe is a bug, not coverage.
