# libs/ui — CLAUDE.md

Application shell: GLFW window + Vulkan renderer + Dear ImGui (docking) +
ImPlot. One subclass of `ws::ui::App` is all an app needs to get a window and
start drawing.

## Purpose
- `ws::ui::App` (`ws/ui/app.hpp`): subclass, override `on_setup()` /
  `on_frame()`, call `run()`. The shell owns the window, the Vulkan context,
  the swapchain, the frame loop and shutdown - apps only draw.
- `ws::ui::AppConfig`: window title, initial size (screen coordinates),
  vsync (false = prefer mailbox/immediate present).

## Allowed dependencies
- `ws::core` (logging), GLFW 3.4, Vulkan via volk (Vulkan-Headers 1.3.296,
  VMA 3.2.1), Dear ImGui 1.91.5 docking (GLFW + Vulkan backends), ImPlot 0.16.
- Nothing else may depend on ws::ui; ws::ui never depends on ws::math.

## Public API
- `App(config)` / `App::run()` → 0 on success, 1 on startup failure (missing
  Vulkan loader/device etc. - details are logged via ws::core).
- `on_setup()`: called once, after the GPU context and ImGui/ImPlot contexts
  are up. Load fonts, tweak style/ConfigFlags here.
- `on_frame()`: called once per frame, between NewFrame and Render. Do NOT
  call `ImGui::Render()` or the backend NewFrame functions - the shell owns
  the frame lifecycle.

## Invariants / rules
- All GLFW/Vulkan state lives in the pimpl (`src/app.cpp`); no Vk*/GLFW types
  in public headers, ever.
- volk meta-loader everywhere: never link the Vulkan loader directly and never
  declare vk entry points outside volk (`VK_NO_PROTOTYPES`).
- VMA's implementation TU is `src/vma_impl.cpp` (dynamic function loading;
  the two loader entry points are handed over from `app.cpp` via
  `VmaAllocatorCreateInfo::pVulkanFunctions` since VMA 3.2 requires it) -
  exactly one `VMA_IMPLEMENTATION` TU in the workspace.
  The allocator is created by the shell for future GPU-side data work; it is
  not yet exposed in the public API.
- Swapchain: `minImageCount + 1` images, BGRA8 preferred, FIFO when vsync,
  mailbox/immediate when not; recreated on resize / out-of-date / suboptimal.
- The ImGui Vulkan backend bakes MinImageCount (= 2, never changes) and its
  per-frame buffer ring at Init; if the swapchain image count or format
  changes, the shell re-initializes the backend.
- `on_frame()` exceptions are caught, logged, and close the window cleanly.
- Don't use the `ImGui_ImplVulkanH_*` example helpers - the backend header
  marks them example-only; the shell manages its own swapchain objects.

## Tests
```
ctest --preset debug -R "^ui$"
```
Window/GPU behavior is not unit-testable headlessly; tests cover only
construction/config. `apps/playground` is the real smoke test.
