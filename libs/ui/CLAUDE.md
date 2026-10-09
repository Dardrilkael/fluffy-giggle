# libs/ui — CLAUDE.md

Guidance for Claude Code when working in this library. `libs/ui` is an
**application shell library**: GLFW window + Vulkan renderer + Dear ImGui
(docking) + ImPlot, wrapped so that a consumer subclassing `ws::ui::App`
gets a window, an event stream, and a place to submit draw commands —
**apps never touch GLFW, Vulkan, or ImGui directly.**

## What this is

- **Windowing** — the platform window (GLFW on desktop, NativeActivity on
  Android), owned entirely by the shell.
- **Event system** — Hazel-style typed events + dispatcher, fed from the
  platform callbacks (GLFW on desktop, NativeActivity on Android), delivered
  to `App::on_event`.
- **Render command queue** — the seam between app logic and GPU work.
- **Render/widget API** — `widgets.hpp` (ImGui/ImPlot-backed draw calls),
  the engine's own vocabulary for drawing, not the graphics API's.
- **2D renderer** — `renderer2d.hpp` (`ws::ui::r2d`): camera'd, textured
  quads and lines, atlas sprites, baked glyph text, a perspective `Camera3D`
  with depth-tested billboards, sorted then batched, flushed under the
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

- Error handling: public APIs return `ws::core::Result`/`Status` (root
  CLAUDE.md, "Error handling") — lib note: the existing surface predates the
  rule (`App::run()` returns int, drop-safe calls log-and-ignore, mid-frame
  Vulkan failures abort); new public APIs follow the rule.

- No third-party header, and no GLFW/Vulkan/ImGui type, may appear under
  `include/` — apps and other libs include `ws/` headers only. `Event`,
  `EventDispatcher`, and `RenderCommandQueue` are plain `ws::ui` types with
  no third-party types in their signatures.
- All GLFW/Vulkan/ImGui state lives behind a pimpl (`src/app.cpp`,
  `src/window_glfw.cpp`/`src/window_android.cpp`) — Vk*/GLFW types never
  appear in public headers, ever.
- GLFW callback functions are `static`, file-local functions in
  `src/window_glfw.cpp` — never declared or forward-declared in a header.
  This is the *only* place GLFW's C callback surface exists.
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
    renderer2d.hpp           # r2d free functions: Camera2D, quads, lines, sprites, fonts + text
    widgets.hpp               # window/text/button/checkbox/slider/plot_line/...
    gui/                      # the immediate-mode gui module (ws::ui::gui, no ImGui)
      gui_context.hpp        # GuiContext + HitTest + Layer: input snapshot, per-pointer hot/active, layer scopes + side tables
      id_stack.hpp           # WidgetId hashing, the scoped id stack, ScopedId RAII, label_display
      input_frame.hpp        # InputFrame: one frame's raw input — mouse, keys, dt_seconds clock, multi-touch table; frame_input() getter
      layout_cursor.hpp      # LayoutCursor: flowing layout + panel stack
      rect.hpp                # Rect + its geometry helpers (contains, edges, centers, expand/shrink) — the shared vocabulary
      theme.hpp               # ChromeTheme: every color/metric the widgets use, + the borrowed font
      widgets.hpp            # button/checkbox/slider/panel/tabs/scroll region — the widget functions + fit_text
  src/
    app.cpp                  # pimpl: Vulkan context, swapchain, frame loop
    atlas_packer.h/.cpp      # pure-CPU shelf packer: atlas pages, sub-rects, gutters
    batch_core.h/.cpp        # pure-CPU batch: chunks, camera VP, geometry (no Vulkan)
    font.h/.cpp              # stb_truetype bake: ASCII 32..126 glyphs, white-RGBA
                             #  coverage, forgiving utf8_next (pure CPU, no renderer state)
    gui.h                    # internal: set_active_input - the shell points frame_input() at its InputFrame; the warn-once reset seam
    gui/                     # the gui module's out-of-line bodies, one concern per file:
      gui.cpp                # shell glue: frame_input()'s live accumulator tap
      gui_context.cpp        # GuiContext: per-pointer hot/active hit-testing, layer scopes + side tables
      gui_id.cpp             # IdStack: FNV-1a scope-path hashing - the label/index/reserved families
      gui_input.cpp          # InputFrame: touch table + the primary-touch -> mouse bridge
      gui_layout.cpp         # LayoutCursor: stacked flow panels
      gui_widgets.cpp        # the widget functions' bodies (lay out, hit-test, draw) + fit_text
    renderer2d.h/.cpp        # r2d engine: command sort/replay + Vulkan flush
    rounded_texture.h/.cpp   # pure-CPU bake of the builtin rounded-rect mask
    shader_provider.h/.cpp   # pipeline shader words: committed spirv.h, or hot
                             # reload when WS_UI_DEV_SHADER_RELOAD (dev only)
    shaders/                  # batch.vert/.frag (GLSL) + spirv.h (committed build input)
    stb_impl.cpp             # sole STB implementation TU (STB_IMAGE + STB_TRUETYPE)
    window.h                 # internal: WindowHost, the platform seam (never reaches apps)
    window_glfw.cpp          # desktop: GLFW window + all glfwSet*Callback installs (only here)
    window_android.cpp       # Android: NativeActivity pump, multi-touch decode, the bridge feed
    widgets.cpp              # ImGui/ImPlot-backed widget implementations
    vma_impl.cpp             # sole VMA_IMPLEMENTATION translation unit
  tests/
```

## Event system 

- `ws/ui/event.hpp`: `Event` base + `EventType` enum (`WindowClose`,
  `WindowResize`, `KeyPressed`, `KeyReleased`, `MouseButtonPressed`,
  `MouseButtonReleased`, `MouseMoved`, `MouseScrolled`, `TouchPressed`,
  `TouchMoved`, `TouchReleased`, `TextInput`). Each event is a
  small POD-ish struct (`WindowResizeEvent{width,height}`,
  `KeyPressedEvent{keycode, repeat}`, etc.) with a `type()` and a `handled`
  flag. The touch trio carries `pointer_id` (the platform's stable
  per-contact id) plus `x`/`y` in framebuffer pixels — the mouse's space;
  `TouchReleasedEvent` adds `cancelled` (the OS aborted the gesture).
  `TextInputEvent{codepoint}` is one UTF-32 codepoint per event, in
  arrival order.
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
  `glfwSetCursorPosCallback`, `glfwSetCursorEnterCallback`,
  `glfwSetScrollCallback`, `glfwSetWindowCloseCallback`) once, in
  `src/window_glfw.cpp` (Android's counterpart is the NativeActivity pump
  in `src/window_android.cpp`).
  Every callback
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
  unconditionally. `Touch*` events are never ImGui-gated (the gate answers
  false for them): on touch platforms ImGui is fed by the primary-touch →
  mouse bridge instead, so apps keep unfiltered finger events.
  `TextInputEvent` gates with `WantCaptureKeyboard`, exactly like
  `KeyPressed`.
- `App::on_event` runs synchronously on the main thread, before
  `on_update()`, once per queued event per frame — no event is ever
  dropped or coalesced except mouse-move/scroll and `TouchMoved`, which
  are coalesced per frame (move keeps the latest position; scroll sums its
  deltas; `TouchMoved` keeps the latest position PER POINTER ID — one
  event per moving contact per frame). `TextInput` never coalesces —
  order is the payload.

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
`ws::ui::r2d` — `begin()` / `begin(camera)` / `begin(Camera3D)` / `end()`,
`draw_quad`, `draw_line` (thickness in px), `draw_sprite`, `draw_rounded_quad`,
`set_layer`, `push_clip` / `pop_clip` (framebuffer-space clip stack),
`load_texture`, `load_atlas`, `load_font`, `draw_text`, `frame_size`,
`sprite_size`, `rgba` (packs 0xRRGGBBAA — NOT IM_COL32 order). Apps record
from `on_update()` only, exactly like widgets.hpp calls; every call is
drop-safe outside a running app (logged once, then ignored). The public
header includes only `<cstdint>`, `<initializer_list>` and `<vector>` — no
Vulkan/GLFW/stb types (stb_image and stb_truetype are PRIVATE, like every
third-party).

- **Recording is data.** A draw appends a `DrawCommand` (sort layer, camera
  block, clip, expanded corners; the material key rides along as metadata).
  `frame_end()` — one point, after `on_update()` returns — stable-sorts the
  frame by (layer, camera block) and replays it into the batch.
  Material is NOT a sort key: the 32-slot table already lets one chunk hold
  many textures, so sorting by it bought nothing and reordered overlapping
  draws that differ in texture (a rounded face painted over its label bar).
  Clip is NOT a sort key either: sorting by clip values swapped the z-order
  of overlapping panels whenever the clip rects compared backwards — the
  replay splits chunks wherever the clip changes in record order instead,
  so the scissor still gets its own chunk with zero reordering (M5).
  Within one layer/block the replay is the exact submission order —
  real painter's order. `set_layer()` is the explicit z escape hatch that
  outranks the key.
- **Clip stack.** `push_clip(x, y, w, h)` pushes a framebuffer-space rect
  (top-left origin, screen space even inside camera blocks); draws snapshot
  the active clip, nesting intersects at push time, and the stack empties
  at every `frame_begin` — a clip never leaks across frames. Clip is not a
  sort key (M5): the replay splits a chunk wherever the active clip changes
  in record order, so interleaved clip runs batch as-is instead of being
  reordered by rect values; the flush sets one `vkCmdSetScissor` per chunk
  (chunk clip intersected with the frame extent, outward-rounded) and skips
  empty-clip chunks entirely. Depth cap 8, balanced overflow like the gui
  stacks: deeper pushes count depth but stop shrinking (warned once per
  renderer), so pops stay paired. `pop_clip()` with nothing pushed is a
  documented no-op. **Record order is the contract**: with clip out of the
  sort, a clip change always splits a chunk — so draw each clipped panel's
  content CONTIGUOUSLY (all of panel A's draws, then all of panel B's);
  A,B,A interleaving costs three chunks instead of two.
- **Chunks (M9: growth, not caps).** One chunk = one VP (64B push
  constants) + one clip (one scissor) + one depth mode (which of the two
  pipelines flushes it) + one texture table + one `vkCmdDrawIndexed`.
  CPU-side, chunks and vertices have NO hard cap anymore: the vectors
  start at `kInitialChunkCapacity`/`kInitialVertexCapacity` and grow
  geometrically at flush — the one point the frame slot is idle (its
  fence was waited in render_frame before flush runs). GPU-side, the
  vertex AND index arenas are per frame slot and grow at the same point
  (the index buffer went per-slot for exactly that reason: a shared
  buffer would be bound by the other slot's in-flight submission), as
  does the chunked tier's per-slot descriptor pool (rebuilt bigger, new
  sets white-inited). The one drop that remains: a quad pushed with NO
  open segment, counted in `BatchCore::dropped_quads()` (reset per
  frame) and warned once per frame by the renderer. A growth failure
  (OOM) logs and skips that frame's draws — loudly, never a silent
  partial memcpy. Texture handling splits by TIER (the device probe in
  app.cpp picks once at init): **bindless** — one global 1024-slot table
  bound once per frame; `tex_index` carries the GLOBAL registry id, so
  texture variety never splits a chunk at all; **chunked** (fallback) —
  a 32-slot table per chunk (`kMaxTexturesPerChunk`); overflow closes
  the chunk and opens one inheriting the segment's VP, clip and depth
  mode.
- **Textures.** `load_texture()` (valid from `on_setup()`) uploads
  synchronously — staging buffer, one-off submit, fence wait. The registry
  only grows (v1 never unloads): ids are indexes, stable for the process
  lifetime, so stale descriptor bindings can never dangle. id 0 is the
  built-in 1x1 white texture; failed loads and unknown ids render white.
  The bindless tier's table is fixed at 1024 slots
  (`batch::kMaxBindlessTextures`): past it a load warns once and hands
  back white — ids must stay dense indexes into the shader's table; the
  chunked tier has no count limit. Table writes: with update-after-bind
  (probed) a new id lands in every frame slot's table immediately, even
  mid-frame; without it each slot takes its writes at its idle flush
  (setup-time loads are in place before the first frame either way).
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
- **Fonts + text.** `load_font(path, pixel_height)` (valid from
  `on_setup()`) bakes one face at one size through stb_truetype — ASCII
  32..126, one `Font` per (face, size), never scaled or re-rasterized at
  runtime. The glyphs pack into the font's OWN atlas pages, never shared
  with `load_atlas` sprites: packing is one-shot and a packed atlas
  never grows, so text merging with icon pages is a non-goal BY DESIGN
  (the batching test pins per-font glyph runs and says so — its comment
  exists so nobody reads that as a regression later). Glyph bitmaps
  bake RGB-white with coverage in alpha, so the vertex tint supplies
  the text color. `draw_text` emits one sprite-quad per visible glyph
  and returns the pen's advance — exactly `Font::measure_text`, by
  construction, for ANY font (empty, degraded); `(x, y)` is the line
  BOX's top-left (baseline = y + ascent internally; `Glyph` offsets
  measure from the line top). No kerning in v1; a codepoint outside the
  bake is no draw and no advance; a page whose upload failed drops its
  quads but keeps the advances honest — layout code never lies.
  `utf8_next` decodes forgivingly: bad bytes and overlong encodings
  become U+FFFD, never a crash or a runaway pointer. The path is the
  CALLER's to resolve — the lib carries no platform knowledge, no
  Windows font directory; the one system-face path constant lives in
  each TU that uses it (playground app, tests). System-font-at-runtime
  is a known future cost: right for a Windows-only tool today, and the
  first thing to revisit if Android ever becomes real.
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
- **Layering.** One render pass (color + depth attachments), three layers:
  r2d flushes right after `vkCmdBeginRenderPass` (2D world and 3D blocks,
  same flush), then ImGui's draw data (widgets), then the command queue
  (overlays).
- **3D camera + depth.** `begin(const Camera3D&)` opens a perspective
  block; inside it the 3D overloads record — `draw_quad(x, y, z, ...)`
  and `draw_sprite(x, y, z, ...)` are camera-facing billboards,
  CENTER-anchored at (x, y, z) with w/h in world units (no rotation in v1;
  the 2D forms stay top-left anchored), and
  `draw_line(x0, y0, z0, x1, y1, z1, ...)` expands to a ribbon of
  WORLD-unit thickness perpendicular to the segment and the view, constant
  along its length. A 3D overload inside a 2D block warns once per frame
  and records nothing; 2D draws inside a 3D block are legal but land on the
  world's z=0 plane through the perspective VP, depth-tested against the
  3D content (surprising — prefer separate blocks). One block is one kind:
  `perspective` rides the begin() epoch (never a sort key), and the
  block's chunks flag `depth_test`, flushing through the depth-tested
  pipeline — LEQUAL + depth-write, so equal depths keep the stable sort's
  submission order; depth clears to 1.0 each frame. 2D, gui and ImGui
  never test or write depth (the 2D pipeline's depth state is
  byte-identical to the pre-3D era), so painter's order is untouched and
  2D chunks after 3D chunks paint over. near_z/far_z are the projection's
  own clipping; `push_clip` stays framebuffer-space and applies to 3D
  unchanged. No OIT in v1 — a semi-transparent depth-writing billboard
  occludes what is behind it; no 3D `draw_text` / `draw_rounded_quad`.
- **Drop-never-defer.** The batch resets everywhere the write queue clears
  (out-of-date acquire, hidden window, abandoned frame); iconified frames
  never reach `on_update()` at all.
- **Descriptor sets (M9: two tiers).** Bindless tier: ONE set per frame
  slot (the 1024-slot global table) from one pool, bound once per flush;
  every slot starts white (written at creation), and new ids land through
  `pending_table_ids_` — immediately with update-after-bind, else at each
  slot's idle flush, made safe by the per-slot `table_written_` high-water
  marks. Chunked tier: one pool + sets per frame slot, grown by rebuild
  at the slot's idle flush (`grow_chunk_sets`); a set is rewritten only
  after its slot's fence was waited and strictly before its own bind.
  Both tiers: sets are keyed by frame slot, allocated at init or at that
  slot's flush, and never touched by a swapchain rebuild; every slot of a
  bound set must hold a valid view, because the shader indexes the
  sampler array dynamically.
- **Swapchain drift.** A same-format resize rebuilds nothing (viewport and
  scissor are dynamic, extents arrive per flush); format drift rebuilds
  both pipelines (the 2D and depth-tested 3D share one create info).
  Image-count drift rebuilds nothing either — the renderer's vertex and
  index arenas and descriptor sets are keyed by frame slot (grown only at
  that slot's idle flush), and swapchain-independent by construction.
- **Shader workflow** (M4). `src/shaders/spirv.h` is generated by
  `tools/compile_shaders.ps1` and committed — the ImGui-backend pattern;
  the default build needs no shader compiler, and that stays true for
  every option below. The flags and the header format live in the script
  ONLY (never duplicate them elsewhere). M9: the header carries THREE
  arrays — vert, chunked frag, bindless frag; `batch.frag` is one source
  with `#ifdef WS_BINDLESS` branches (a 1024-slot table +
  `nonuniformEXT` vs the 32-slot plain form), the script compiles the
  bindless variant with `-DWS_BINDLESS --target-env vulkan1.1`
  (nonuniformEXT emits SPIR-V 1.3) while the chunked variant keeps the
  original flags byte-pinned, and the dev hot reload compiles both frag
  variants all-or-nothing. When a shader changes:
  - Regenerate: `cmake --build --preset debug --target ws_ui_regen_shaders`
    (or `tools\compile_shaders.ps1` directly), then commit the header.
  - Check staleness: `--target ws_ui_check_shaders` regenerates to a
    temp copy and fails with a diff if the committed header is stale (a
    machine with no compiler prints SKIPPED and passes; set
    `WS_GLSLANG_EXE` or `WS_GLSLANG` to pin one). Line endings are
    normalized before comparing — git may hand out CRLF.
  - `WS_UI_REGEN_SHADERS=ON` (default OFF) makes `ws_ui` depend on the
    check target, so a stale header fails the build.
  - Compiler resolution (script, CMake targets, and dev reload agree):
    explicit `-Compiler`/`WS_GLSLANG_EXE`, then `WS_GLSLANG`, then
    glslang/glslangValidator/glslc on PATH, then `VULKAN_SDK\Bin`
    (or newest `C:\VulkanSDK`), then the known
    `%TEMP%\glslang-16.6.0` drop. The output is pinned to **glslang
    16.6.0** — a different version warns (never fails the check) since
    the SPIR-V embeds the generator version; the drop is
    `glslang-16.6.0-windows-x86_64-release.zip` from the KhronosGroup
    glslang GitHub release 16.6.0, unzipped into `%TEMP%\glslang-16.6.0`.
  - Dev hot reload (`WS_UI_DEV_SHADER_RELOAD=ON`, default OFF, Debug +
    desktop only — configure fails otherwise): `src/shader_provider.*`
    polls the GLSL mtimes each frame and, on change, recompiles with
    glslang (same flags/resolution), validates the SPIR-V magic word,
    and rebakes the pipelines once every frame slot's fence is waited (the
    slot fences cover every submitted draw; deferred per-frame deletion
    queues wait for M12). A failed compile logs the compiler's
    stderr and keeps the last good module — never a crash, never a null
    VkShaderModule. With the option OFF the provider is a trivial wrapper
    over the committed words; no process-spawning code is compiled in.

## Immediate-mode gui (`ws::ui::gui`)

`include/ws/ui/gui/` is the dependency-free widget layer over r2d — the
ImGui-shaped part of the stack with zero ImGui in it. Immediate-mode: the
app re-describes its screen every frame by calling widget functions; the
functions hold no state of their own, everything persistent lives in the
GuiContext side tables — the exact shape a future retained-mode tree could
drive unchanged (that boundary is deliberate; don't paint it over).

- **One seam.** A widget call does all four concerns under one id at once:
  identity (`ids().id(label)` — the id stack's scope hashed in),
  hit-testing (`GuiContext::hit_test(id, rect)`), layout
  (`LayoutCursor::next(w, h)`) and draw emission (r2d at the caller's
  layer). What a widget draws, where it hit-tests and what it returns
  cannot drift apart — they come from the same rect and the same id.
- **Input.** The shell accumulates raw input into one `InputFrame` per
  frame (`frame_input()`, drop-safe headless) — parallel to the
  ImGui-filtered `Event` stream, so gui widgets see the mouse even where
  an ImGui window sits. `dt_seconds` is the frame clock: the SHELL writes
  it once per pump (`App`'s main loop, a `steady_clock` delta) and
  `next_frame()` clears edges but never it — the shell's next write is
  the refresh — so hand-built frames and the headless `frame_input()`
  read 0. The frame carries the mouse, the key state and
  the multi-touch table (`touches`, up to `kMaxTouches` = 8 contacts,
  keyed by the platform's stable pointer ids; an entry lives through
  its release frame, so a widget can still see who lifted). Same-frame
  id reuse — a finger lifts and the platform hands its id to a new
  contact before `next_frame()` (Android recycles low ids fast) — is
  order-exact in the flags: the slot reads pressed+released with down,
  so the gui reports the lift while the new contact claims fresh.
  `touch_down` clears the edges only when it CREATES an entry (a
  recycled slot's stale release never leaks into a new contact);
  position is last-write, the mouse's own snapshot rule.
  `gather_pointers()` flattens the two into one uniform list — slot 0 is
  the mouse (id `kMousePointerId`) while `mouse_present`, the touches
  follow in arrival order — and `pointer_pos(id)` looks one up (an
  absent mouse answers false). `mouse_present` is the platform's own
  statement that a mouse exists at all, held state the frame boundary
  keeps: the desktop cursor callbacks set it on move, button or cursor
  enter (each sampling the position, so a first click lands where the
  user sees it, not at the (0,0) default) and clear it when the cursor
  leaves the window; on Android only the real-mouse (Bluetooth) path
  touches it — hover-enter/move/buttons set it, HOVER_EXIT clears it.
  While false the mouse is not a pointer: no hover, no hot, no press,
  no click, no wheel — a touch-only device (where `mouse_x/y` mirror
  the last finger or sit at (0,0)) can never show hover at the
  top-left, and a cursor that left the window takes its in-flight
  press with it (the track drops with the pointer; hover dies the same
  frame, not one later). The `touch_*` writers
  maintain the primary-touch → mouse bridge: the FIRST contact of a
  gesture mirrors the mouse's position, button 0 and its edges (no
  promotion on lift — single-finger use, the mouse path and ImGui keep
  working, unchanged) — raw fields only, NEVER `mouse_present`: a
  finger is not a mouse, and a lifting one must not unplug a real one.
  `begin_frame(frame_input())` adopts the snapshot;
  widgets run; `end_frame()` resolves the frame per pointer (a release
  whose owner vanished is dropped, not misattributed — but a
  re-claimed same-frame id reuse keeps its claim: the release edge
  belonged to the lift that reported it).
- **Identity.** `WidgetId` is an FNV-1a hash of the id-stack scope plus a
  family tag and the label — same label, same scope, same id, stable
  across frames (that stability IS widget identity). `id(label)` speaks
  the two marker spellings: a `##` suffix ids the FULL string but never
  draws — `label_display()` cuts the display at the first marker —
  while `###` REDIRECTS the id to the marker onward, one identity
  behind two displays (`Play###pause` == `Pause###pause`, so a widget
  can relabel without orphaning its state); plain labels hash exactly
  as before. `internal_id(key)` is the reserved third family ('R' —
  the gui's own state keys, "scroll"/"drag") that no user label can
  spell; changing a reserved key orphans the old-keyed state,
  deliberately. Loop-generated
  children must `ids().push(index)` between calls or they collide; the
  context censuses ids per frame (M7: a fixed 512-slot open-addressing
  table stamped by frame generation — cleared by bumping the stamp, so
  nothing allocates and capacity never frees; a frame past 512 unique
  ids warns once and stops censusing the rest) and warns once per
  duplicate.
- **Hot/active, per pointer.** One `PointerTrack` per pointer, matched
  across frames by pointer id (that carry IS the drag). The classic
  rules run per pointer: press-inside sets that pointer's active
  (pointer capture: press feedback and drag tracking continue off-rect
  until release); release-inside = `clicked`, release-outside clears
  with no click. `hit_test` OR-folds the per-pointer claims into one
  `HitTest` for the widget — a two-finger press on one widget is one
  press, and one click — and `HitTest::pointer` reports the owning
  pointer as the lowest slot (the mouse outranks touches, so a bridged
  finger resolves to the mouse it mirrors — when a mouse is present at
  all: absent, the finger is the fold's only entry and owns the press
  by itself). Capture is per pointer: a
  drag on one slider does not stop another finger from pressing the
  next. `hovered` is the SETTLED visual (M7): it reports the pointer's
  last frame's final hot, gated on this frame's inside-ness — so an
  overlapped widget never lights up, not even on the covering frame
  (one frame of settling at worst; the live this-frame claim stays
  visible through `hot_id()`). Hover is never a click precondition —
  clicks are pure spatial containment — and a touch has no hover at
  all: contact IS presence.
- **Layers (M7).** One layer concept across hit-testing and drawing:
  `begin_layer(Layer)`/`end_layer()` scope a stratum — `Layer::base` 10
  (the default), `popup` 20, `tooltip` 30, `modal` 40 — and
  `begin_layer` drives `r2d::set_layer` with the same number, so the
  batch's sort order and the hit rules can never disagree about who is
  on top. A strictly higher layer wins hot whatever the record order
  (draw order must not decide the pointer) and steals a press claimed
  THIS frame by a lower one — a click under a popup belongs to the
  popup. Within one layer the LATER widget wins BOTH hot and a fresh
  press: painter order is the topmost pixel, the one the user sees, so
  the widget that shows hover is the one that clicks — the two must
  never disagree on an overlap (M10; the steal leaves the earlier
  widget with a stale pressed for the steal frame only, and its release
  reports nothing). A drag carried from an earlier
  frame is capture: never stolen, not even by a modal veil opening
  mid-drag (the gesture finishes on its owner). `Layer::modal` raises a
  VEIL that is frame-wide: it starts at the frame's first modal scope,
  outlives that scope's `end_layer`, and lifts at the next
  `begin_frame` — while it is up, every strictly lower stratum is
  blocked (no hover, no hot, no new press; no click-through), and the
  modal's own chrome hit-tests at the modal stratum, unblocked. The
  veil owns the keyboard as well (see Widget state + focus):
  registrations under it are refused, and the frame it lifts hands the
  displaced focus back. The
  frame-wide veil is what lets an app draw its chrome FIRST — the
  stratum sorts it above base content wherever it records — while the
  widgets it blocks still hit-test later in the frame; open the modal
  scope before the widgets it blocks and the block lands on the very
  frame the modal appears. Scopes are frame-scoped like every gui
  stack (a leaked `begin_layer` is reset by the next `begin_frame`),
  capped at depth 8 with the balanced-clamp overflow, and every pop is
  a no-op on an empty stack.
- **Hit clips (the clip-aware hit seam).** `push_hit_clip(rect)`/
  `pop_hit_clip()`/`current_hit_clip()` on GuiContext — the
  hit-testing twin of r2d's clip stack: `hit_test` intersects its rect
  with the innermost clip, so a widget the renderer's clip hides (a row
  scrolled past a viewport) cannot claim hover, hot or a press from
  whatever sits over or under the panel, and a half-visible widget
  claims its visible part. The panels push it at the SAME rect they
  hand `r2d::push_clip` (`begin_panel`: the content rect;
  `begin_scroll_panel`: the viewport) and pop beside `r2d::pop_clip`;
  a raw `r2d::push_clip` does NOT hit-clip — the gui mirrors at the
  panel seams. Nesting intersects at push time (the stored entry IS
  the effective clip), frame-scoped like every gui stack, depth 8 with
  the balanced clamp, pop-on-empty a no-op. Capture is never clipped:
  a press claimed while visible stays owned when its widget scrolls
  out of sight (the clip gates new claims, not in-flight gestures),
  and it clicks only if the release ends inside the claimable part.
  Fully-clipped widgets still census their id (duplicate detection
  among scrolled-out rows is intact).
- **Widget state + focus (M8).** Three distinct concepts: hot = under the
  pointer, active = being pressed, focused = receives keyboard input
  (`focused_` on GuiContext, beside hot/active). `state(id, bounds,
  disabled=false)` is the one call widgets use instead of raw hit_test:
  it runs exactly ONE hit_test (never a second path) and returns
  `WidgetResult{hit, state}` — the hit plus the `WidgetState` derived off
  it, so the two can never disagree. Priority is transient-first:
  Disabled beats everything, then Pressed, Hovered; Focused is the
  RESTING keyboard state — a focused widget under the pointer reports
  Hovered and draws its ring off `is_focused()` instead. `disabled`
  short-circuits BEFORE the hit-test: a disabled widget claims nothing
  (not even a census entry — the widget under the same pointer may
  claim) and drops held focus; the ambient `begin_disabled` scope joins
  the parameter — either locks the widget. Focus follows the PRESS
  EDGE: a fresh
  press on a widget registered focusable this frame
  (`register_focusable(id)`, call order = Tab order, before state())
  takes the focus right there — a press that slips off and never clicks
  keeps the focus it took; a press edge that lands on no focusable
  (empty space, a plain widget) clears the focus at end_frame. Tab /
  Shift+Tab (detected at end_frame off `keys::kTab` + shift held) and
  `nav_next()`/`nav_previous()` queue moves resolved at end_frame as
  one net step against the frame's registrations, wrapping; focus
  outside the list restarts forward at the first, backward at the last.
  A raised modal veil owns the keyboard like the pointer: widgets under
  it never register — the refusal IS the focus trap, Tab wraps within
  the modal's own registrations and cannot escape underneath — and
  `state()` drops the focus of a widget that hit-tests under it. The
  veil's GENUINE rise stashes the focus it displaces (`veil_upon_entry_`
  tells lift from continuation; a continuing modal never re-stashes),
  and the frame the veil lifts hands the focus back — but only on a
  quiet frame (no press, no nav): a press or a Tab move on the closing
  frame keeps the focus it earned. Two adjacent modals are one
  continuous veil stretch — one stash, one restore. Otherwise focus
  persists while the widget keeps running:
  an id, not a claim — an absent widget keeps focus until a press
  elsewhere or a Tab move takes it (the app decides whether that is
  what it wants). `register_focusable` refuses three ways: the sentinel
  and past-cap ids warn once and drop; inside a `begin_disabled` scope
  and under a raised veil it refuses SILENTLY, both routine. The
  registration caps at `kMaxFocusables` = 128 with the
  census's warn-once degrade; the registration-order list is the seam a
  directional/gamepad nav would extend (register bounds alongside the
  id) — deliberately deferred, not blocked. `keys::` (input_frame.hpp)
  is the spelling of the keys with built-in behavior — Tab, Enter,
  Space, the shifts, the arrows (kLeft/kRight step the focused slider)
  — GLFW numbering, so core and widgets can never
  disagree.
- **Disabled scopes (M10).** `begin_disabled(condition = true)`/
  `end_disabled()`/`disabled()` — an ambient lock around whatever runs
  between them: widgets become dead with no per-widget plumbing. The
  condition form is a plain `if` written as a scope (a false condition
  pushes a bit that locks nothing, so the matching `end` never eats an
  enclosing true scope — the mask is the OR of every open condition,
  `uint32_t` bit per level with a depth counter). Pass-through is the
  point: a locked widget claims nothing, so the click lands on
  whatever sits underneath — the scope withdraws hover, hot, presses
  AND registration, and `state()` reads the lock (the parameter and
  the scope are two doors into the same Disabled state). Frame-scoped
  like every gui stack (begin_frame clears a stranded scope), balanced
  clamp at 32 like the layer/id stacks (deeper scopes keep counting
  and warn once per process), every `end_disabled()` a no-op on an
  empty stack. Layer scopes RESET the lock at `begin_layer` and
  restore it at `end_layer`: a dialog opened from a locked form is
  live chrome, not more dead form — and the restore means the lock
  never leaks into a stratum it did not come from.
- **Press cancel.** `cancel_pointer(id)` ends that pointer's press with
  no click: the active claim clears, the track flags `cancelled`, and
  it claims nothing until it lifts (`end_frame` lifts it with the
  release, so the platform can reuse the id). The scroll-drag path is
  today's caller (see Widgets). Cancelling a bridged touch cancels the
  mouse it mirrors, and vice versa — the mirror rule. The bridge's
  duplicate itself is benign by the fold rules above: the OR-fold
  yields one press and one click, and the lowest-slot rule resolves
  drags to the same finger.
- **Persistent state.** `persistent_state<T>(WidgetId)` returns a typed
  side-table entry keyed (type, id) — how a scroll offset survives frames
  and the panel vanishing for frames. Never a local static: if a widget
  needs to remember, it keys here. Eviction is EXPLICIT (M14):
  `clear_persistent_state()` empties every table,
  `clear_persistent_state(WidgetId)` one widget's entries in every
  table — there is deliberately no automatic one. Call between frames
  (mid-frame, a scroll panel holds a live reference into its entry);
  a cleared widget's state re-creates fresh on its next call.
- **Layout.** `LayoutCursor` is a flowing cursor with a panel stack:
  `next(w, h, align = top)` places at the cursor — `Align::center`/
  `bottom` place within the line height SO FAR (the widget counts when
  it is the tallest, so a tall latecomer tops out at the line's top
  and earlier widgets never re-center — place the tallest first) —
  and advances below the line's tallest plus the panel's spacing;
  `same_line(gap = -1)` rewinds onto the current line, resuming one
  panel-spacing right of the last widget (an explicit gap — tab_bar's
  flush 0 — overrides for that one seam); `begin_panel(bounds, padding,
  spacing = 0)` pushes a content rect (bounds inset by the padding,
  width/height clamped at 0 — a bounds too small for its padding is an
  empty panel, never a backwards one), `end_panel` pops it; nesting
  parks the parent's cursor. `remaining_width()` is the room left on
  the current line — the fill-width answer: a button/slider handed a
  NEGATIVE width fills to the content's right edge. The gui's own
  panels pass `theme.spacing`, so their children flow spaced between
  lines and between same_line() items (`theme.spacing = 0` restores
  the unspaced pitch); `flow_extent()` counts the between-line gaps
  but never a trailing one. All four gui
  stacks (ids, panels, renderer clips, hit clips) cap at depth 8 with
  BALANCED overflow — depth keeps counting past the clamp, so a matching
  pop never
  eats a real ancestor — and every pop is a documented no-op on an empty
  stack, so unbalanced calls degrade, never corrupt.
- **Theme.** `ChromeTheme` — the faces and the metrics (control height,
  corner radius, padding, spacing, scroll speed, drag threshold, and
  the M14 label/scrollbar metrics: `label_char_width` — the fontless
  6-px-per-byte estimate, `label_inset`, `check_inset`,
  `slider_handle_width`, `scrollbar_thumb_width`,
  `scrollbar_band_width`, `scrollbar_min_thumb`) in one
  plain struct,
  `kDefaultChrome` constexpr. `control_disabled` is the one dead face
  every widget's Disabled state wears (it covers idle, hover and press
  — a locked control has no live reactions), and `disabled_alpha`
  (0.45) fades everything drawn on top of it: labels, checkbox fill,
  slider fill and handle, the selected tab's accent. Widgets take a
  `const ChromeTheme&` per
  call, so a panel can carry its own variant inline. `scaled(scale)`
  returns a copy with every metric multiplied — the touch-density move,
  fed by `App::ui_scale()` (Android display density / 160, desktop
  content scale); faces and the font pointer carry unchanged (the caller
  scales the font bake and panel rects by the same factor). `dim(color,
  factor)` (beside the struct, not scaled — a ratio, not a metric) is
  the helper that does the fading: it multiplies a color's ALPHA only,
  so a dimmed label keeps its hue. `font` is a
  borrowed `const r2d::Font*` (null = the v1 label bars, byte-for-byte
  the pre-font look): the theme does NOT own the font — load it once
  (`r2d::load_font`, valid from `on_setup()`) and keep it alive as
  long as any theme pointing at it is in use. A second face (a heading
  weight, say) grows a sibling field, not a rework of the pointer.
- **Widgets** — all caller-state, all drawing through r2d, all drop-safe
  headless, all fitted (M14): every label runs through `fit_text` — the
  `##` display cut, then the truncating width pass, the ONE fitter for
  both sizing and drawing, so a row's width and what it draws cannot
  disagree — and (M10) all citizens: every interactive widget registers
  focusable and drives through `state()` — focus, the 1px accent ring,
  keyboard control and the Disabled look are one story everywhere, not
  a button privilege: `button` (optional atlas icon + label; Enter/
  Space activate the focused one exactly like a click; a negative
  width fills the rest of the line),
  `checkbox`
  (box + label, auto-sized row; Enter/Space toggle the focused one),
  `slider_float`/`slider_int`
  (click-to-set and drag are one rule: the owner maps the pointer while
  pressed; kLeft/kRight step the focused one — 1% of the range per
  press, int by 1, clamped to [min, max], changed reported only on a
  real move; a negative width fills the rest of the line),
  `begin_panel`/`end_panel` (chrome + clip + id scope + layout panel
  flowing theme.spacing apart), `tab_bar` (the caller's int IS the
  selection; the strip
  selects, the per-tab content is the caller's branch; the tabs
  register in strip order and Enter/Space select the focused one),
  `begin_scroll_panel`/`end_scroll_panel` (children flow inside a
  virtual panel shifted by the offset — they draw and hit-test at
  scrolled positions — while the renderer clip and its hit-testing twin
  stay at the unscrolled viewport, so a row poked past the edge neither
  draws nor claims;
  the offset is persistent state, the content extent is measured at the
  previous end: one frame of lag, by design. Scrolling is ARBITRATED
  (M12): every panel — plain or scrollable — registers its whole bounds
  as a wheel claimant each frame (`register_wheel_claim`; plain panels
  claim with a null key, scroll panels with their ScrollState's
  address), and end_frame resolves layer-first (a popup eats the wheel
  over the panel below), then innermost bounds (nested panels stop
  double-scrolling), then registration order, dropping every claim
  under a raised modal veil — the winner publishes for the NEXT frame,
  the same one-frame settle `hot_prev` answers for hover, so the first
  notch after the cursor arrives over a panel settles rather than
  scrolls. A PRESENT mouse gates the whole wheel device. The touch
  drag is singular the same way: a fresh press inside the viewport
  claims a per-pointer scroll ARM (`claim_scroll_arm`, later-or-higher
  wins, held press-to-release) and past `theme.drag_threshold` the
  scroll owns the finger — `cancel_pointer` fires before children run
  that frame, and the offset follows the contact 1:1 with no
  crossing-frame jump; a panel whose watched track lost the arm drops
  it silently, so the innermost panel the press landed on is the only
  one that drags. The veil gates the raw-input gestures through
  `claims_open()` exactly as it gates hit_test. Tab lands in view: the
  frame after the keyboard focus moves onto a widget inside the panel,
  the panel shifts the offset the minimum that pulls the widget into
  the viewport (`focus_into_view_pending`/`focused_bounds`). The
  scrollbar thumb is GRABBABLE — the whole right gutter is the hit
  band, a press there ends any child press beneath it, a press on the
  thumb keeps the grab offset, a press off it jumps the thumb centered
  under the pointer, and the drag maps 1:1 through the track. Known
  cosmetic limits: a drag-tracking child (a slider) can move <
  threshold px before the cancel fires, and the thumb grab's cancel
  lands one frame after the child claimed — the same class of lag).
- **v1 limits, deliberate:** labels draw through the theme font's
  glyphs when one is set, the v1 quad bars otherwise — and both go
  through `fit_text`, so a label wider than its budget truncates on a
  character boundary (never splitting a UTF-8 codepoint) to the
  longest fitting prefix + `...` instead of dropping or spilling;
  the gui is screen-space only (the same
  space layout and hit-testing use); scroll is vertical-only (the wheel
  and the touch drag are both arbitrated — see Widgets); panels are
  chrome, not input surfaces.
- Everything tests headless (r2d draws drop without a window): the widget
  suite lives in `tests/test_ui.cpp` and runs with the rest via
  `ctest --preset debug -R ui`. `apps/playground` carries the full
  composition demo plus the Phase 10 "net inspector" screen — a
  feedforward net drawn in the world layer, driven live by a gui panel —
  and the multi-touch demo: pointer-id-colored trails under every
  finger, two sliders dragged at once, a two-finger pinch-zoom composed
  app-side off the raw `frame_input()` table, finger-scrolling, and a
  text echo of `TextInputEvent` codepoints. On desktop the touch
  overlay simply draws nothing — there are no touches to show.

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
- `ui_scale()`: the platform's ui scale — Android display density / 160,
  desktop the window content scale (1.0 at 100%). Multiply theme metrics
  (`ChromeTheme::scaled`), font pixel sizes and panel rects by it for a
  finger-sized gui on a phone; valid from `on_setup()` on, 1.0 before
  `run()`.
- `widgets.hpp`: the drawing API for apps — `begin_window`/`end_window`,
  `text`, `button`, `checkbox`, `slider_float`/`slider_int`, `separator`,
  `same_line`, `begin_plot`/`end_plot`/`plot_line`, `show_imgui_demo`/
  `show_implot_demo`.
- `renderer2d.hpp` (`ws::ui::r2d`): the 2D drawing API — `begin`/`end`
  camera blocks, `begin(const Camera3D&)` perspective blocks (inside them
  the 3D overloads of `draw_quad`/`draw_sprite`/`draw_line` — the forms
  with a z — record depth-tested billboards), `draw_quad` (top-left anchor,
  center-pivot rotation), `draw_line`, `draw_sprite` (atlas sub-rect
  quads), `draw_rounded_quad`
  (9-sliced builtin mask, radius clamped to half the shorter side),
  `set_layer`, `push_clip`/`pop_clip` (nested framebuffer clip rects),
  `load_texture`, `load_atlas`, `load_font` (bakes one face at one pixel
  size — ASCII 32..126, the font's own atlas pages), `draw_text` (glyph
  quads; returns the advance, exactly `Font::measure_text`), `frame_size`,
  `sprite_size`. Draws are valid only inside `on_update()`, within a
  `begin()` block.
- `gui/` (`ws::ui::gui`): the immediate-mode widget layer — `GuiContext` +
  `frame_input()` (the frame's raw input: mouse — fields plus
  `mouse_present`, the platform's own yes/no on the device's existence,
  without which the mouse is not a pointer at all —, the frame clock
  `dt_seconds` (the shell's one write per pump), keys and the
  multi-touch
  table — `touches`/`touch_count`, the platform-facing `touch_*` writers,
  `gather_pointers`, `pointer_pos`, `bridged_touch`), `LayoutCursor`,
  `IdStack` (and `ScopedId`, its RAII scope guard) — `id(label)`'s
  `##`/`###` marker semantics, `internal_id(key)` (the reserved
  state-key family) and `label_display(label)` (the display cut) —
  `LabelFit`/`fit_text` (the shared label fitter: display cut,
  truncating width pass; never allocates — the caller's scratch),
  `clear_persistent_state` (the explicit eviction, by id or wholesale),
  `ChromeTheme`/
  `kDefaultChrome`, and `button`/`checkbox`/
  `slider_float`/`slider_int`/`begin_panel`/`end_panel`/`tab_bar`/
  `begin_scroll_panel`/`end_scroll_panel`, plus layer scopes — the
  `Layer` strata (base/popup/tooltip/modal) with `begin_layer`/
  `end_layer`/`current_layer`, one numbering for both hit-testing and
  r2d draw order. Hit clips: `push_hit_clip(rect)`/`pop_hit_clip()`/
  `current_hit_clip()` narrow what `hit_test` claims — the
  hit-testing twin of `r2d::push_clip`, pushed by the panels at the
  same rect (a scrolled-out row cannot claim); capture is never
  clipped. Widget state + keyboard focus: `state(id, bounds,
  disabled)` (one hit-test, `WidgetResult` = hit + `WidgetState`),
  `register_focusable(id)` (call order = Tab order; refused inside a
  disabled scope and under a raised modal veil), `is_focused(id)`,
  `set_focus(id)` (kInvalidWidgetId clears), `nav_next()`/
  `nav_previous()` (queued, resolved at end_frame), the `keys::`
  constants for the built-in key spellings (Tab, Enter, Space, the
  shifts, the arrows). Disabled scopes: `begin_disabled(condition)`/
  `end_disabled()`/`disabled()` — the ambient lock (claims, focus and
  registration withdrawn inside; layer scopes reset it at `begin_layer`
  and restore it at `end_layer`). Hot/active are per pointer:
  `hot_id()`/`active_id()` default to the mouse and take a pointer id;
  `is_hot(id, pointer)`/`is_active(id, pointer)` are the am-I spelling
  (the invalid id pinned to false); `owning_pointer(widget_id)` and
  `pointer(id)` inspect the fold; `cancel_pointer(id)` ends a press
  with no click. Gesture arbitration (M12): `register_wheel_claim`/
  `wheel_won(key)` (panels claim the wheel; the one-frame settle),
  `claim_scroll_arm`/`scroll_arm_owner(pointer)` (the per-pointer
  touch-drag arm, press-to-release), `claims_open()` (the modal veil's
  gate, the same answer hit_test and the raw-input gestures read),
  `focus_into_view_pending()`/`focused_bounds()` (Tab's
  scroll-into-view handoff). Valid only between
  `GuiContext::begin_frame` and
  `end_frame`, inside the caller's r2d
  screen-space block; the state is the caller's, persistence lives in
  the context's side tables (`persistent_state<T>`, evicted only by the
  explicit `clear_persistent_state`), and `reset_warn_once()` re-arms
  the once-per-process warns - the test seam.

## Backend invariants

- volk meta-loader everywhere: never link the Vulkan loader directly and
  never declare vk entry points outside volk (`VK_NO_PROTOTYPES`).
- The batch shader indexes its texture array with a runtime index (both
  tiers' variants), which the spec gates behind
  `shaderSampledImageArrayDynamicIndexing`: device selection skips GPUs
  lacking it and `vkCreateDevice` enables the feature. Universally
  supported on desktop GPUs, so the gate costs nothing.
- **Texture tiers (M9).** `create_device` probes descriptor indexing and
  picks once: bindless when the device has
  `shaderSampledImageArrayNonUniformIndexing` +
  `descriptorBindingPartiallyBound` (+
  `descriptorBindingSampledImageUpdateAfterBind` when present) AND the
  limits cover one 1024-slot sampled-image binding (UAB limits when UAB
  is wanted, core limits otherwise). The probe runs over the 1.1+ core
  `vkGetPhysicalDeviceFeatures2`/`Properties2` pNext chains — the 1.2
  dialect or `VK_EXT_descriptor_indexing` on 1.1, checked BEFORE any
  pNext query so a pre-1.2 device never meets an unknown struct — and
  enables through the `VkDeviceCreateInfo` pNext. Every miss (1.0 loader
  or device, missing feature, missing limit) falls back to the chunked
  tier, never a failure; the instance requests the loader's own version
  (`vkEnumerateInstanceVersion`) so a 1.1+ device is reachable at all.
  The tier logs one `core::info` line at startup (the smoke marker) and
  is fixed for the process.
- VMA's implementation TU is `src/vma_impl.cpp` (dynamic function loading;
  the two loader entry points are handed over from `app.cpp` via
  `VmaAllocatorCreateInfo::pVulkanFunctions` since VMA 3.2 requires it) —
  exactly one `VMA_IMPLEMENTATION` TU in the workspace. The allocator is
  created by the shell for future GPU-side data work; it is not yet
  exposed in the public API.
- Swapchain: `minImageCount + 1` images, BGRA8 preferred, FIFO when vsync,
  mailbox/immediate when not; recreated on resize / out-of-date /
  suboptimal — resize is driven by the same `WindowResizeEvent` path, not a
  separate poll. An acquire returning `VK_SUBOPTIMAL_KHR` is still a
  success — the frame renders and presents (only the submit's wait retires
  the semaphore the acquire signaled; destroying a still-signaled semaphore
  would violate the spec), and the shell rebuilds once that frame has
  retired; `VK_ERROR_OUT_OF_DATE_KHR` drops the frame instead.
- Frames in flight (M6): `r2d::kMaxFramesInFlight` (= 2) frame slots — each
  owns a fence, command pool/buffer and acquire semaphore — bound how many
  frames may overlap, decoupled from the swapchain image count; each
  swapchain image instead owns its render-finished semaphore. A frame
  waits its slot's fence BEFORE acquiring (the CPU throttle), and the
  fence is reset only after the acquire succeeded — an out-of-date bail
  leaves the slot signaled and pristine, so no acquire can ever wait a
  fence nobody submits against. Two in-flight frames always render
  different images (acquire never hands out an image whose present is
  still pending), so a per-image semaphore can never be signaled twice
  while pending; present waits the image's rendered semaphore.
- The render pass carries explicit external subpass dependencies: color
  (`EXTERNAL` → `COLOR_ATTACHMENT_OUTPUT` — pairs with acquire's wait
  stage) and depth (`EXTERNAL`/`COLOR_ATTACHMENT_OUTPUT` ↔
  `EARLY/LATE_FRAGMENT_TESTS` with `DEPTH_STENCIL_ATTACHMENT_WRITE`,
  first-use and last-render chained) — without them the single shared
  depth image is a WAW hazard across frames in flight. That dependency
  chain IS the proof the shared image is safe; per-frame depth images
  would only buy back a small serialization bubble.
- Validation is opt-in via `WS_UI_VALIDATION` (env var — deliberately not
  public API): the Khronos validation layer + `VK_EXT_debug_utils` are
  enabled when both enumerate, messages route through the ws logger
  (error/warn/info by severity), and a machine without the Vulkan SDK
  warns once and runs without. Synchronization validation only ever runs
  under that opt-in (SDK ≥ 1.3.239 has it on by default).
- The ImGui Vulkan backend bakes MinImageCount (= 2, never changes) and its
  per-frame buffer ring at Init; if the swapchain image count or format
  changes, the shell re-initializes the backend.
- Depth attachment: unconditional from the first `create_swapchain` — the
  render pass is always color + depth, so the ImGui backend's Init-baked
  pipeline stays render-pass-compatible across every rebuild. The format
  is chosen ONCE in `select_device` (D32_SFLOAT → D24_UNORM_S8_UINT →
  D16_UNORM, first with DEPTH_STENCIL_ATTACHMENT support in optimal
  tiling; startup fails loudly if the device has none), which runs
  exactly once per process and strictly before any render pass exists —
  that ordering IS the compatibility guarantee. Depth clears to 1.0 each
  frame (the LEQUAL convention). r2d builds TWO pipelines against that
  pass — depth-off (byte-identical to the pre-depth 2D era: test off,
  write off, ALWAYS) and depth-on (LEQUAL + write, so ties keep the stable
  sort's order) — sharing one layout and descriptor sets; `flush` binds
  per chunk on kind change, after the skip checks.
- `on_event()` and `on_update()` exceptions are caught, logged, and close
  the window cleanly (an `on_update()` exception abandons the frame — no
  `Render()`, since the app may have thrown between `Begin`/`End`).
- Mid-frame Vulkan failures are logged and `std::abort()` — after a failed
  submit there is no state to roll back to (semaphores in flight, a command
  buffer half recorded); the same policy as Dear ImGui's own examples.
  Startup failures instead return 1 from `App::run()`.
- Don't use the `ImGui_ImplVulkanH_*` example helpers — the backend header
  marks them example-only; the shell manages its own swapchain objects.
- Input platform invariants. Desktop is mouse-only BY DECISION — no Win32
  WM_POINTER: touch screens keep the synthesized single-mouse behavior.
  Desktop owns `mouse_present` through the cursor callbacks: a move, a
  button or a cursor-enter sets it and samples the position (the first
  click after the window opens lands at the real cursor, not (0,0)); the
  cursor leaving the window clears it — from that frame the gui's
  pointer list has no mouse (hover dies the same frame, a held press
  evaporates, the wheel is dead). Android decodes EVERY contact (pointer
  index from the action bits,
  MOVE updates all pointers, CANCEL lifts them all cancelled), emits
  the `Touch*` events plus the bridge's `Mouse*` off `bridged_touch()`,
  and drops the 9th+ finger with a once-per-frame warn; the touch path
  never touches `mouse_present` — only the real-mouse (Bluetooth) path
  does (hover-enter/move/buttons set it with the position, HOVER_EXIT
  clears it), so a touch-only device lists touches only and a finger
  can never unplug a real mouse. On Android,
  ImGui is fed BY THE BRIDGE — `io.AddMousePosEvent`/
  `AddMouseButtonEvent` direct; touch-source motion never reaches
  `ImGui_ImplAndroid_HandleInputEvent`, whose index-0 following is what
  produced the old stuck-press bug. Android soft-keyboard IME is out of
  scope v1 (NativeActivity `hasCode=false` cannot receive IME commits —
  revisit when the gui grows a text widget); `TextInputEvent` flows from
  GLFW chars on desktop and hardware-key taps on Android.

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