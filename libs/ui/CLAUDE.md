# libs/ui — CLAUDE.md

Guidance for Claude Code when working in this library. `libs/ui` is an
**application shell library**: GLFW window + Vulkan renderer + Dear ImGui
(docking) + ImPlot, wrapped so that a consumer subclassing `ws::ui::App`
gets a window, an event stream, and a place to submit draw commands —
**apps never touch GLFW, Vulkan, or ImGui directly.**

## What this is

- **Windowing** — GLFW-backed window, owned entirely by the shell.
- **Event system** — Hazel-style typed events + dispatcher, fed from GLFW
  callbacks, delivered to `App::on_event`.
- **Render command queue** — the seam between app logic and GPU work.
- **Render/widget API** — `widgets.hpp` (ImGui/ImPlot-backed draw calls),
  the engine's own vocabulary for drawing, not the graphics API's.
- **2D renderer** — `renderer2d.hpp` (`ws::ui::r2d`): camera'd, textured
  quads and lines, atlas sprites, sorted then batched, flushed under the
  widgets.
- **Immediate-mode gui** — `gui/` headers (`ws::ui::gui`): dependency-free
  widgets (button/checkbox/slider/panel/tabs/scroll region) that draw
  through r2d; caller-owned state, identity + hit-testing + layout + draw
  stay one seam.
- **`ws::ui::App`** — the shell itself: owns the window, Vulkan context,
  swapchain, frame loop, event pumping, and shutdown.

The shell is the product. Consumers link against this library and only
ever see `ws::ui` types (`App`, `Event`, `EventDispatcher`,
`RenderCommandQueue`, `widgets::*`, `r2d::*`) — never `VkInstance`,
`GLFWwindow*`, `ImGuiContext*`, or any other third-party type.

## Hard rule: no leaking internal dependencies

This is the single most important architectural constraint of this library.

- No third-party header, and no GLFW/Vulkan/ImGui type, may appear under
  `include/` — apps and other libs include `ws/` headers only. `Event`,
  `EventDispatcher`, and `RenderCommandQueue` are plain `ws::ui` types with
  no third-party types in their signatures.
- All GLFW/Vulkan/ImGui state lives behind a pimpl (`src/app.cpp`,
  `src/window.cpp`) — Vk*/GLFW types never appear in public headers, ever.
- GLFW callback functions are `static`, file-local functions in
  `src/window.cpp` — never declared or forward-declared in a header. This
  is the *only* place GLFW's C callback surface exists.
- Before adding any public API, check: *could a consumer compile this
  header without Vulkan/GLFW/ImGui installed?* If not, it's a leak — fix it
  before merging.
- `widgets.hpp` wraps ImGui/ImPlot calls in `src/widgets.cpp`; when an app
  needs one more widget, add it there instead of exposing a third-party
  header.

## Module layout

```
libs/ui/
  include/ws/ui/
    app.hpp                 # App, AppConfig — PUBLIC, zero third-party includes
    event.hpp                # Event, EventType, typed events, EventDispatcher
    render_command_queue.hpp # RenderCommandQueue
    renderer2d.hpp           # r2d free functions: Camera2D, quads, lines, sprites, textures
    widgets.hpp               # window/text/button/checkbox/slider/plot_line/...
    gui/                      # the immediate-mode gui module (ws::ui::gui, no ImGui)
      gui_context.hpp        # GuiContext: input snapshot + hot/active + side tables
      id_stack.hpp           # WidgetId hashing + scoped id stack
      input_frame.hpp        # InputFrame: one frame's raw input; frame_input() getter
      layout_cursor.hpp      # LayoutCursor: flowing layout + panel stack
      rect.hpp                # Rect, WidgetId, HitTest — the shared vocabulary
      theme.hpp               # ChromeTheme: every color/metric the widgets use
      widgets.hpp            # button/checkbox/slider/panel/tabs/scroll region — the widget functions
  src/
    app.cpp                  # pimpl: Vulkan context, swapchain, frame loop
    atlas_packer.h/.cpp      # pure-CPU shelf packer: atlas pages, sub-rects, gutters
    batch_core.h/.cpp        # pure-CPU batch: chunks, camera VP, geometry (no Vulkan)
    gui.h/.cpp               # gui module internals: input accumulator, persistent state
    gui_widgets.cpp          # the widget functions' bodies (lay out, hit-test, draw)
    renderer2d.h/.cpp        # r2d engine: command sort/replay + Vulkan flush
    rounded_texture.h/.cpp   # pure-CPU bake of the builtin rounded-rect mask
    shaders/                  # batch.vert/.frag (GLSL) + spirv.h (committed build input)
    stb_impl.cpp             # sole STB_IMAGE_IMPLEMENTATION translation unit
    window.h                 # internal: WindowEventSink (never reaches apps)
    window.cpp               # GLFW window + all glfwSet*Callback installs (only here)
    widgets.cpp              # ImGui/ImPlot-backed widget implementations
    vma_impl.cpp             # sole VMA_IMPLEMENTATION translation unit
  tests/
```

## Event system 

- `ws/ui/event.hpp`: `Event` base + `EventType` enum (`WindowClose`,
  `WindowResize`, `KeyPressed`, `KeyReleased`, `MouseButtonPressed`,
  `MouseButtonReleased`, `MouseMoved`, `MouseScrolled`). Each event is a
  small POD-ish struct (`WindowResizeEvent{width,height}`,
  `KeyPressedEvent{keycode, repeat}`, etc.) with a `type()` and a `handled`
  flag.
- `EventDispatcher`: constructed with an `Event&`, exposes
  `dispatch<T>(fn)` — calls `fn(T&)` only if the event's runtime type
  matches `T`, and sets `handled = fn`'s return value. Apps write one
  `on_event` and dispatch to typed handlers instead of a switch:

```cpp
void on_event(ws::ui::Event& e) override {
    ws::ui::EventDispatcher d(e);
    d.dispatch<ws::ui::WindowResizeEvent>([this](auto& ev){ return on_resize(ev); });
    d.dispatch<ws::ui::KeyPressedEvent>([this](auto& ev){ return on_key(ev); });
}
```

- The shell installs the GLFW callbacks (`glfwSetWindowSizeCallback`,
  `glfwSetKeyCallback`, `glfwSetMouseButtonCallback`,
  `glfwSetCursorPosCallback`, `glfwSetScrollCallback`,
  `glfwSetWindowCloseCallback`) once, in `src/window.cpp`. Every callback
  builds the matching `Event` and forwards it to a single
  `std::function<void(Event&)>` stored on the window. Zero-size resize
  events (minimize on Windows) are suppressed — nothing can be sized
  against them, and the restore reports the real size.
- ImGui gets first refusal: the shell always calls the ImGui GLFW backend's
  own callbacks first (or lets `ImGui_ImplGlfw_InstallCallbacks` chain them)
  so widgets consume input before the app sees it. If
  `ImGuiIO::WantCaptureMouse`/`WantCaptureKeyboard` is set, the shell marks
  the corresponding `Event` `handled = true` before it reaches
  `App::on_event`, but still delivers `WindowResizeEvent`/`WindowClose`
  unconditionally.
- `App::on_event` runs synchronously on the main thread, before
  `on_update()`, once per queued event per frame — no event is ever
  dropped or coalesced except mouse-move/scroll, which are coalesced per
  frame (move keeps the latest position; scroll sums its deltas).

## Render command queue

- `ws/ui/render_command_queue.hpp`: `RenderCommandQueue` is a bump-allocated
  ring buffer of submitted commands (`submit<T>(fn)` type-erases a
  callable + its captured args into the buffer; `execute()` runs and clears
  them in submission order). This is the seam between "app logic" and
  "GPU work" — apps never issue Vulkan calls themselves.
- Two queues, swapped per frame (classic double-buffer): the app thread
  (or `on_update`) submits into the write queue; the shell's render step
  swaps and executes the read queue against the current command buffer.
  A frame that cannot render (out-of-date acquire, hidden window) clears
  the write queue instead of deferring — a command never outlives the
  frame it was submitted in. Single-threaded today, but the split means
  moving `on_update()` to its own thread later doesn't change the
  app-facing API.
- `widgets.hpp` calls still record directly against ImGui's immediate-mode
  context — ImGui already batches its own draw lists — but any future
  non-ImGui GPU work (custom overlays, offscreen passes) goes through the
  command queue, never straight Vulkan calls from app code.

## 2D renderer (r2d)

`ws/ui/renderer2d.hpp` is the engine-style drawing API: free functions in
`ws::ui::r2d` — `begin()` / `begin(camera)` / `end()`, `draw_quad`,
`draw_line` (thickness in px), `draw_sprite`, `draw_rounded_quad`,
`set_layer`, `push_clip` / `pop_clip` (framebuffer-space clip stack),
`load_texture`, `load_atlas`, `frame_size`, `sprite_size`,
`rgba` (packs 0xRRGGBBAA — NOT IM_COL32 order). Apps record from
`on_update()` only, exactly like widgets.hpp calls; every call is drop-safe
outside a running app (logged once, then ignored). The public header
includes only `<cstdint>`, `<initializer_list>` and `<vector>` — no
Vulkan/GLFW/stb types (stb_image is PRIVATE, like every third-party).

- **Recording is data.** A draw appends a `DrawCommand` (sort layer, camera
  block, clip, expanded corners; the material key rides along as metadata).
  `frame_end()` — one point, after `on_update()` returns — stable-sorts the
  frame by (layer, camera block, clip) and replays it into the batch.
  Material is NOT a sort key: the 32-slot table already lets one chunk hold
  many textures, so sorting by it bought nothing and reordered overlapping
  draws that differ in texture (a rounded face painted over its label bar).
  Within one layer/block/clip the replay is the exact submission order —
  real painter's order. `set_layer()` is the explicit z escape hatch that
  outranks the key.
- **Clip stack.** `push_clip(x, y, w, h)` pushes a framebuffer-space rect
  (top-left origin, screen space even inside camera blocks); draws snapshot
  the active clip, nesting intersects at push time, and the stack empties
  at every `frame_begin` — a clip never leaks across frames. Clip sits
  BEFORE material in the sort key so all draws under one clip batch into a
  contiguous run (the 8-chunk cap makes interleaved runs unaffordable);
  the flush sets one `vkCmdSetScissor` per chunk (chunk clip intersected
  with the frame extent, outward-rounded) and skips empty-clip chunks
  entirely. Depth cap 8, balanced overflow like the gui stacks: deeper
  pushes count depth but stop shrinking (warned once per renderer), so
  pops stay paired. `pop_clip()` with nothing pushed is a documented no-op.
- **Chunks.** Up to 8 CPU chunks per frame: 32 textures / 131072 vertices
  each. One chunk = one VP (64B push constants) + one clip (one scissor)
  + one 32-slot texture table (one descriptor set) + one `vkCmdDrawIndexed`.
  Texture overflow closes the chunk and opens a new one that inherits the
  segment's VP and clip — no mid-frame GPU work ever. Caps exhausted:
  further draws drop, one warn per frame.
- **Textures.** `load_texture()` (valid from `on_setup()`) uploads
  synchronously — staging buffer, one-off submit, fence wait. The registry
  only grows (v1 never unloads): ids are indexes, stable for the process
  lifetime, so stale descriptor bindings can never dangle. id 0 is the
  built-in 1x1 white texture; failed loads and unknown ids render white.
- **Atlases.** `load_atlas({...})` (valid from `on_setup()`) packs an image
  set into 2048x2048 pages — shelf packing, tallest first, 1px
  duplicated-edge gutters so magnified sprites never sample a shelf
  neighbour — and uploads one texture per page. `Sprite{page, index}`
  keys into per-page rect tables (page is the page texture's id): a
  page's sprites ride one texture-table slot, so however interleaved the
  calls are they never split a chunk — and, texture not being a sort key,
  they keep the app's draw order too; a full page spills to a new page
  and batches apart, degrading gracefully. Missing/failed images become
  `Sprite{}` (white — the load_texture contract); `sprite_size()` reports
  native pixels (packing never scales). Page tables only grow, the same
  contract as the texture registry.
- **Rounded quads.** `draw_rounded_quad` 9-slices a builtin 32×32 mask
  texture (`src/rounded_texture.*`, baked analytically: one-texel coverage
  antialiasing across the corner arcs, RGB white — shape rides in alpha, so
  the vertex tint supplies the color). Uploaded at GPU init, right after
  white; the four corner slices scale the baked 8 px arcs to the requested
  radius, the strips and center stretch the solid interior. The radius
  clamps to half the shorter side; a sub-pixel radius degrades to the
  plain square quad (minifying the arc would look worse). Rotation pivots
  the whole rect's center (`batch::quad_corners_pivot`), so the nine
  slices move as one rigid shape. Degenerate strips record zero-area
  quads — a rounded quad is always exactly nine records. All nine share
  one material: one batch segment, not nine. A failed mask upload warns
  once and degrades every rounded quad to square corners (white texture).
- **Layering.** One render pass, three layers: r2d flushes right after
  `vkCmdBeginRenderPass` (2D world), then ImGui's draw data (widgets), then
  the command queue (overlays).
- **Drop-never-defer.** The batch resets everywhere the write queue clears
  (out-of-date acquire, hidden window, abandoned frame); iconified frames
  never reach `on_update()` at all.
- **Descriptor sets.** One set per (swapchain image, chunk slot), written
  only after that image's fence is waited and strictly before its own bind;
  all sets start fully white because the fragment shader indexes the
  sampler array dynamically — every slot of a bound set must be valid.
- **Swapchain drift.** A same-format resize rebuilds nothing (viewport and
  scissor are dynamic, extents arrive per flush); format drift rebuilds the
  pipeline, image-count drift the sets and per-image vertex regions.
- `src/shaders/spirv.h` is generated by `tools/compile_shaders.ps1` and
  committed — the ImGui-backend pattern; the build needs no shader compiler.

## Immediate-mode gui (`ws::ui::gui`)

`include/ws/ui/gui/` is the dependency-free widget layer over r2d — the
ImGui-shaped part of the stack with zero ImGui in it. Immediate-mode: the
app re-describes its screen every frame by calling widget functions; the
functions hold no state of their own, everything persistent lives in the
GuiContext side tables — the exact shape a future retained-mode tree could
drive unchanged (that boundary is deliberate; don't paint it over).

- **One seam.** A widget call does all four concerns under one id at once:
  identity (`ids().current(label)` — the id stack's scope hashed in),
  hit-testing (`GuiContext::hit_test(id, rect)`), layout
  (`LayoutCursor::next(w, h)`) and draw emission (r2d at the caller's
  layer). What a widget draws, where it hit-tests and what it returns
  cannot drift apart — they come from the same rect and the same id.
- **Input.** The shell accumulates raw input into one `InputFrame` per
  frame (`frame_input()`, drop-safe headless) — parallel to the
  ImGui-filtered `Event` stream, so gui widgets see the mouse even where
  an ImGui window sits. `new_frame(frame_input())` adopts the snapshot;
  widgets run; `end_frame()` resolves the frame (a release whose owner
  vanished is dropped, not misattributed).
- **Identity.** `WidgetId` is an FNV-1a hash of the id-stack scope plus a
  family tag and the label — same label, same scope, same id, stable
  across frames (that stability IS widget identity). Loop-generated
  children must `ids().push(index)` between calls or they collide; the
  context censuses ids per frame and warns once per duplicate.
- **Hot/active.** Press-inside sets active (pointer capture: press
  feedback and drag tracking continue off-rect until release);
  release-inside = `clicked`, release-outside clears active with no
  click. `hovered` is hot-gated, so widgets under a foreign drag don't
  light up.
- **Persistent state.** `persistent_state<T>(WidgetId)` returns a typed
  side-table entry keyed (type, id) — how a scroll offset survives frames
  and the panel vanishing for frames. Never a local static: if a widget
  needs to remember, it keys here.
- **Layout.** `LayoutCursor` is a flowing cursor with a panel stack:
  `next(w, h)` places at the cursor and advances below the line's
  tallest, `same_line()` rewinds onto the current line, `begin_panel`
  pushes a content rect (bounds inset by the theme padding),
  `end_panel` pops it; nesting parks the parent's cursor. All three gui
  stacks (ids, panels, renderer clips) cap at depth 8 with BALANCED
  overflow — depth keeps counting past the clamp, so a matching pop never
  eats a real ancestor — and every pop is a documented no-op on an empty
  stack, so unbalanced calls degrade, never corrupt.
- **Theme.** `ChromeTheme` — the faces and the metrics (control height,
  corner radius, padding, spacing, scroll speed) in one plain struct,
  `kDefaultChrome` constexpr. Widgets take a `const ChromeTheme&` per
  call, so a panel can carry its own variant inline.
- **Widgets** — all caller-state, all drawing through r2d, all drop-safe
  headless: `button` (optional atlas icon + label bar), `checkbox`
  (box + label bar, auto-sized row), `slider_float`/`slider_int`
  (click-to-set and drag are one rule: the owner maps the pointer while
  pressed), `begin_panel`/`end_panel` (chrome + clip + id scope + layout
  panel), `tab_bar` (the caller's int IS the selection; the strip
  selects, the per-tab content is the caller's branch),
  `begin_scroll_panel`/`end_scroll_panel` (children flow inside a
  virtual panel shifted by the offset — they draw and hit-test at
  scrolled positions — while the clip stays at the unscrolled viewport;
  the offset is persistent state, the content extent is measured at the
  previous end: one frame of lag, by design).
- **v1 limits, deliberate:** labels are quad bars tracking the label
  length (r2d has no font yet); the gui is screen-space only (the same
  space layout and hit-testing use); scroll is vertical-only and every
  viewport under the pointer wheels; hit-testing ignores clips (a row
  scrolled out of sight still hit-tests — that needs a clip-aware hit
  seam, not a widget patch); panels are chrome, not input surfaces.
- Everything tests headless (r2d draws drop without a window): the widget
  suite lives in `tests/test_ui.cpp` and runs with the rest via
  `ctest --preset debug -R ui`. `apps/playground` carries the full
  composition demo plus the Phase 10 "net inspector" screen — a
  feedforward net drawn in the world layer, driven live by a gui panel.

## Public API

- `App(config)` / `App::run()` → 0 on success, 1 on startup failure (missing
  Vulkan loader/device etc. — details are logged via `ws::core`).
- `AppConfig`: window title, initial size (screen coordinates), vsync
  (false = prefer mailbox/immediate present).
- `on_setup()`: called once, after the GPU context and ImGui/ImPlot contexts
  are up. Load fonts, tweak style/ConfigFlags here.
- `on_event(Event&)`: called once per queued input/window event, before
  `on_update()` for that frame. Default implementation is empty.
- `on_update()`: called once per frame, between NewFrame and Render. Submit
  widget calls and command-queue work here. Do NOT call `ImGui::Render()`
  or the backend NewFrame functions — the shell owns the frame lifecycle.
- `widgets.hpp`: the drawing API for apps — `begin_window`/`end_window`,
  `text`, `button`, `checkbox`, `slider_float`/`slider_int`, `separator`,
  `same_line`, `begin_plot`/`end_plot`/`plot_line`, `show_imgui_demo`/
  `show_implot_demo`.
- `renderer2d.hpp` (`ws::ui::r2d`): the 2D drawing API — `begin`/`end`
  camera blocks, `draw_quad` (top-left anchor, center-pivot rotation),
  `draw_line`, `draw_sprite` (atlas sub-rect quads), `draw_rounded_quad`
  (9-sliced builtin mask, radius clamped to half the shorter side),
  `set_layer`, `push_clip`/`pop_clip` (nested framebuffer clip rects),
  `load_texture`, `load_atlas`, `frame_size`, `sprite_size`. Draws are
  valid only inside `on_update()`, within a `begin()` block.
- `gui/` (`ws::ui::gui`): the immediate-mode widget layer — `GuiContext` +
  `frame_input()`, `LayoutCursor`, `IdStack`, `ChromeTheme`/
  `kDefaultChrome`, and `button`/`checkbox`/`slider_float`/`slider_int`/
  `begin_panel`/`end_panel`/`tab_bar`/`begin_scroll_panel`/
  `end_scroll_panel`. Valid only between `GuiContext::new_frame` and
  `end_frame`, inside the caller's r2d screen-space block; the state is
  the caller's, persistence lives in the context's side tables.

## Backend invariants

- volk meta-loader everywhere: never link the Vulkan loader directly and
  never declare vk entry points outside volk (`VK_NO_PROTOTYPES`).
- The batch shader indexes `u_textures[32]` with a runtime index, which the
  spec gates behind `shaderSampledImageArrayDynamicIndexing`: device
  selection skips GPUs lacking it and `vkCreateDevice` enables the feature
  (previously `pEnabledFeatures` was null — all features off — and the
  shader silently depended on driver leniency). Universally supported on
  desktop GPUs, so the gate costs nothing.
- VMA's implementation TU is `src/vma_impl.cpp` (dynamic function loading;
  the two loader entry points are handed over from `app.cpp` via
  `VmaAllocatorCreateInfo::pVulkanFunctions` since VMA 3.2 requires it) —
  exactly one `VMA_IMPLEMENTATION` TU in the workspace. The allocator is
  created by the shell for future GPU-side data work; it is not yet
  exposed in the public API.
- Swapchain: `minImageCount + 1` images, BGRA8 preferred, FIFO when vsync,
  mailbox/immediate when not; recreated on resize / out-of-date /
  suboptimal — resize is driven by the same `WindowResizeEvent` path, not a
  separate poll.
- The ImGui Vulkan backend bakes MinImageCount (= 2, never changes) and its
  per-frame buffer ring at Init; if the swapchain image count or format
  changes, the shell re-initializes the backend.
- `on_event()` and `on_update()` exceptions are caught, logged, and close
  the window cleanly (an `on_update()` exception abandons the frame — no
  `Render()`, since the app may have thrown between `Begin`/`End`).
- Don't use the `ImGui_ImplVulkanH_*` example helpers — the backend header
  marks them example-only; the shell manages its own swapchain objects.

## When implementing new features

1. Design the public API first, backend-agnostic, and sanity-check it
   against the "no leaking internal dependencies" rule above.
2. Implement the GLFW/Vulkan/ImGui-specific pieces under `src/`.
3. Route any new GPU work through `RenderCommandQueue` (or the r2d API for
   2D drawing) rather than calling Vulkan directly from app-facing code.
4. Confirm a consumer could include only `include/ws/ui/*.hpp` headers and
   never need Vulkan/GLFW/ImGui headers at compile time (only at link time,
   transitively, for the final binary).

## Tests
```
ctest --preset debug -R ui
```