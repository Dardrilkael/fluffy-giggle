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
- **`ws::ui::App`** — the shell itself: owns the window, Vulkan context,
  swapchain, frame loop, event pumping, and shutdown.

The shell is the product. Consumers link against this library and only
ever see `ws::ui` types (`App`, `Event`, `EventDispatcher`,
`RenderCommandQueue`, `widgets::*`) — never `VkInstance`, `GLFWwindow*`,
`ImGuiContext*`, or any other third-party type.

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
    widgets.hpp               # window/text/button/checkbox/slider/plot_line/...
  src/
    app.cpp                  # pimpl: Vulkan context, swapchain, frame loop
    window.h                 # internal: WindowEventSink (never reaches apps)
    window.cpp               # GLFW window + all glfwSet*Callback installs (only here)
    widgets.cpp              # ImGui/ImPlot-backed widget implementations
    vma_impl.cpp             # sole VMA_IMPLEMENTATION translation unit
  tests/
```

## Event system (Hazel-style)

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

## Backend invariants

- volk meta-loader everywhere: never link the Vulkan loader directly and
  never declare vk entry points outside volk (`VK_NO_PROTOTYPES`).
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
3. Route any new GPU work through `RenderCommandQueue` rather than calling
   Vulkan directly from app-facing code.
4. Confirm a consumer could include only `include/ws/ui/*.hpp` headers and
   never need Vulkan/GLFW/ImGui headers at compile time (only at link time,
   transitively, for the final binary).

## Tests
```
ctest --preset debug -R ui
```