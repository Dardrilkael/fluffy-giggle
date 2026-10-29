# UI Task — Stage 14/15 Working Plan

Status: **complete** (finished 2026-10-29). Every block landed as its
own commit; the checklist at the bottom is the record. The stage-15
acceptance is `test_ui_shell` (headless, the r2d stub) plus
`apps/shell_example`, and the whole-workspace configure the stage-14
commits had silently broken (see the fixes note at the bottom) is
green again.

The stages below continue the `ws::ui` arc (stages 1-13 are merged; see
`libs/ui/CLAUDE.md` for their summary).

## Stage 14: canvas layer and canvas view (UI side)

- `Canvas2D` interface, backend-agnostic: lines and polylines (joins
  miter/round/bevel, caps butt/round/square), dashed and dotted strokes,
  arcs, circles, ellipses, rounded rects, quadratic/cubic beziers, filled
  polygons (convex and simple concave), text, images, clip push/pop,
  transform stack. Strokes are anti-aliased with widths in screen-space
  pixels (independent of zoom). Curve flattening picks segment count from
  the screen-space size.
- `Renderer2DCanvas` implementation over the existing renderer. Add
  whatever primitives the renderer lacks (thick-line tessellation, AA
  edges, dash generation) inside `ws::ui`; if Part A already added
  thick-line and dash helpers, reuse them.
- `CommandListCanvas`: a recording implementation for tests (assert
  emitted primitives, no GPU).
- Text measurement API on the canvas so host logic never touches the font
  system.
- Geometry helpers in `ws::math`: point-to-segment distance,
  point-in-polygon, polyline distance and nearest point, rect/segment
  intersection, AABB union and expand.
- `ViewTransform2D`: pan, zoom (and optional rotation) with
  world<->screen mapping, zoom-at-point, zoom limits, fit-to-rect,
  animated transitions (needs Clock). Adaptive grid renderer (major/minor
  lines or dots, fades by zoom, snaps in world units).
- `CanvasView` widget: allocates a rect, applies a configurable pan/zoom
  input policy (wheel, middle drag, space+drag, touch pinch/pan), hands a
  `Canvas2D&` and the transform to a host draw callback, and supports
  retained layers (offscreen cache of static content, invalidated by
  version counters) with a documented fallback if offscreen rendering is
  unsupported.
- Tests: stroke tessellation output against expected vertex counts and
  winding, join/cap geometry, flattening tolerance, world/screen
  round-trip, zoom-at-point invariant (the point under the cursor does
  not move), grid line spacing at many zoom levels, all geometry helpers
  including degenerate input.
- Gallery page: a canvas with every primitive, joins/caps/dashes, a
  zoomable infinite grid, a stress test of 100k primitives with a
  frame-time readout.

### Stage 14 design notes (decisions taken)

- New namespace `ws::ui::canvas`; headers under `include/ws/ui/canvas/`,
  sources under `src/canvas/` — they join the `ws_ui_gui` pure archive
  (draw through the r2d free-function seam, no Vulkan/GPU link dep).
- r2d gains ONE primitive: `draw_vertex_quad(pos[4][2], uv[4][2],
  rgba[4][4], texture)` — a free-form quad with per-corner color. Triangles
  are degenerate quads (corner duplicated), matching the fixed
  0,1,2/2,3,0 index pattern; `DrawCommand` grows from one packed color to
  one per corner.
- AA is CPU-tessellated: cores at full alpha plus 1px fringe quads whose
  corner alpha ramps 1 -> 0 (coverage is 50% at the geometric edge).
  Hairlines (width <= 1px) are two alpha-ramped quads.
- Stroke widths and AA fringes are SCREEN pixels (independent of camera
  zoom and canvas transform scale); dash lengths are PATH-LOCAL units
  (the SVG convention). The canvas multiplies the camera zoom (set by
  `CanvasView` via `set_view_scale`) with its own transform scale.
- All tessellation is pure and public (`canvas/tessellation.hpp`) so the
  tests can assert vertex counts and winding directly.
- Retained layers: r2d v1 has no offscreen render target, so a retained
  layer is a CPU-side `CommandList` cache (record once, replay through the
  canvas each frame) invalidated by a version counter — the documented
  fallback; the GPU offscreen cache is where a future render-target
  extension plugs in (same invalidation contract).

## Stage 15: workspace, panel framework and AppShell

Needs Part A Stage 7 (docking) and Stage 6 (property grid), plus Stages
1-6 of this task. Do not start until both are confirmed merged.

**Prerequisite gap found in this repo (2026-10-29):** neither Part A's
docking nor its property grid exists here — the only docking in tree is
ImGui's own backend, and no property-grid widget has landed. Decision:
build minimal in-repo versions as part of this stage (a split-tree dock
host with tabbed leaves, splitters and json serialization; a
schema-driven settings/property grid over the existing widget rows) and
mark the seam clearly so a future full Part A can replace them.

- Panel registration API: id, title, icon, default dock placement, draw
  callback, `onFocus`/`onClose` hooks, multi-instance support, a
  per-panel shortcut context, and an optional per-panel settings scope.
  Focus and active-panel tracking; panel-scoped shortcut contexts hook
  into Stage 5.
- Layouts: per-user and per-project persistence via Stage 2 and Part A's
  dock serialization; named layouts (save, restore, reset to default);
  an automatic "Window" menu listing panels and layouts.
- `AppShell`: ties it all together into one reusable skeleton. Menu bar
  and toolbar from the registry, dock space, status bar, command palette,
  toast host, modal host, window title with dirty state, drag-and-drop
  of files, recent files, autosave/crash recovery, settings panel
  auto-generated from the schema (property grid), theme and font from
  settings with live reaction, a job-progress indicator in the status bar
  that lists running jobs with cancel, and clean shutdown (cancel jobs,
  flush settings, save layout).
- A document abstraction: `Document` interface (path, dirty flag via undo
  save-point, load/save through Result, per-document job group,
  per-document undo stack), plus `DocumentManager` (open, close with a
  save/discard/cancel prompt, activate, multiple documents, restore-
  session list).
- The shell contains no product-specific code. A host app registers
  commands, panels, a document type, and a settings schema, and
  everything else is provided.
- Tests: panel registration and layout restore with missing panels,
  document close-with-dirty flow, session restore, shutdown ordering
  (jobs cancelled before panels destroyed), shell startup with an empty
  config directory and with a corrupt one.
- `apps/shell_example`: a minimal app (a text-and-plot "scratchpad")
  built only from the shell: two panels (an editor and a plot fed by a
  background job), a document type, a few commands, a settings schema.
  It is the acceptance test of the whole task.

### Stage 15 design notes (decisions taken)

- New sources under `src/shell/` join the `ws_ui_gui` archive too; the
  archive gains a PUBLIC `ws::platform` dep (headless services: recent
  files, autosave, title; the OS halves stay in `ws_platform`'s win/
  android TUs). `AppShell` never calls a `make_*` factory — the host
  injects `FileDialogs`/`MessageBoxes` interfaces (fake-friendly).
- `AppShell` is a plain class the host's `App` subclass owns and drives
  from `on_update()`; it never touches `ws::ui::App` itself, so the whole
  shell remains headless-testable.
- Panel-scoped shortcuts: `ws::app`'s `KeyContext::panel` rung is the
  context; a panel-scoped command's `enabled` predicate is gated on the
  shell's active panel (the resolver stays untouched).

## Progress checklist

- [x] Stage 14 — geometry helpers in ws::math (point_segment, polyline,
      point_in_polygon, segment_clip_rect, bounds; + Vec2 cross,
      right_normal, unpack_color, arc_step for the canvas)
- [x] Stage 14 — r2d per-corner-color primitive (draw_vertex_quad,
      already in renderer2d.hpp)
- [x] Stage 14 — tessellation core + tests (stroke/fill/flatten/dash,
      math-lib vector math, Vec2[4]/Vec4[4] seam)
- [x] Stage 14 — Canvas2D / CommandListCanvas / Renderer2DCanvas
      (interface + recording sink for tests + r2d-driving sink)
- [x] Stage 14 — ViewTransform2D + adaptive grid + CanvasView + tests
      (world/screen round-trip, zoom-at-point invariant, fit, pan_by,
      grid spacing adapts to zoom; CanvasView with wheel/middle-drag/
      space+drag/touch-pinch input policy, retained-layer API stub)
- [x] Stage 14 — gallery canvas page (apps/ui_gallery: every primitive,
      joins/caps/dashes, zoomable infinite grid, 100k stress test with
      frame-time readout)
- [x] Stage 15 — property grid + tests (schema-driven widget rows:
      bool→checkbox, int+range→slider_int, int+enum→combo, real+range→
      slider_float, text→text_input, text+enum→combo; grouped by
      category with collapsing headers; PropertyGridState holds the
      per-category open-state + per-text-setting input buffers)
- [x] Stage 15 — dock host (split tree + serialization) + tests
- [x] Stage 15 — panel framework + tests
- [x] Stage 15 — Document/DocumentManager + session restore + tests
- [x] Stage 15 — AppShell + shutdown ordering + tests
- [x] Stage 15 — apps/shell_example
- [x] docs refresh (CLAUDE.md files) + final zip

## Stage 15 as-built notes (2026-10-29)

- `include/ws/ui/shell/` + `src/shell/` (both joined the `ws_ui_gui`
  pure archive, which gained PUBLIC deps `ws::math` - the canvas
  layer's public Vec2 - and `ws::platform` - the shell's headless
  services): `dock.hpp` (the split tree: add/remove/activate,
  resolve() -> leaves + splitter bands, apply_drag with
  kMinWeight-normalized clamps, tolerant json round-trip that drops
  unknown panel ids), `panels.hpp` (PanelRegistry + PanelHost owning
  the tree: single/multi-instance ids, focus with on_focus/on_close,
  cycling), `document.hpp` (the Document interface - dirty via the
  undo save point - and DocumentManager: open/adopt, the
  ClosePlan/finish_close flow, session save/restore, per-document
  Autosave pairs with probe/recover/discard), `shell.hpp` (AppShell:
  the menu bar with the automatic Window menu - panel toggles, layout
  save/restore/reset, recents submenu - the toolbar, the dock view
  with tab strips and draggable splitters, status-job rows with
  cancel, the close-with-dirty 3-button modal, the recovery prompt,
  the named-layout prompt, the palette, the toasts, the ordered
  shutdown: cancel_jobs -> close_panels -> autosaves -> session ->
  layout -> recents -> flush_settings).
- `test_ui_shell` (libs/ui/tests/test_ui_shell.cpp): the dock
  geometry/serialization/clamp/prune cases, the panel validation and
  focus cases, the document close-with-dirty and session-restore
  cases, the autosave snapshot/recover/discard case, and the shell
  cases - first-run startup, corrupt config startup, layout restore
  with missing panels, the dirty title, the panel key rung, palette
  dispatch, drops, the live theme key, and the shutdown ordering
  (cancel_jobs precedes close_panels).
- `apps/shell_example`: the scratchpad - a .txt document type with
  whole-content undo commands, an Editor panel (append/remove-line
  rows + the undo state), a Plot panel (a JobSystem job computing a
  damped sine, the status-bar row with cancel, bars through r2d),
  app.undo/app.redo/app.clear/app.about/app.replot beside the shell's
  File/Window set, and the settings schema (shell.theme, plot.points,
  plot.amplitude) the built-in settings panel renders. The host loads
  the system face (bounce's rule: `C:/Windows/Fonts/segoeui.ttf` on
  desktop, Roboto under Android) and installs it on the theme it
  hands `AppShell::draw` each frame - the shell's stock themes are
  constexpr and can never carry the pointer, and a fontless theme is
  the v1 label bars by contract, which is what the example showed
  before the wiring landed (every string a white placeholder bar).

## Latent breaks the full-workspace configure found (all fixed)

The stage-14/15 work had only ever been built through the spice-only
presets; the whole-workspace configure (the `debug` preset) had gone
quietly broken and nobody had run it:

- `ws_add_test(ui_canvas ...)` linked `ws::ui_canvas`, a target that
  never existed - `ws_add_test` grew a `LINK <target>` override and
  the canvas/shell tests now link `ws::ui_gui` (the pure archive they
  always meant to name).
- `ws_ui_gui` never declared its `ws::math` dep even though the
  canvas layer's public headers include `ws/math/vecmath.hpp`.
- The gallery's stage-14 canvas page used `LayoutCursor::cursor_x/
cursor_y`, members that were never public - the cursor gained the
  `cursor_x()/cursor_y()/advance()` API the page always needed.
- `apps/CMakeLists`' `ws_app_wants` checked the APP NAME against the
  lib list (every call site spells "app, libs..."), so in any subset
  build only an app named like its lib (spice) could ever configure;
  the app name is now skipped, and playground's gate lists its
  `audio` dep like the contract says.
- The desktop-Linux configure (a dev-box convenience, not a shipped
  platform) can now build the headless halves: `ws_platform` drops
  the win/android OS TUs there, `ensure_directory` moved to
  paths.cpp (a pure std::filesystem utility that used to drag
  `os_location` into every headless link), and `test_platform` gates
  to the platforms whose OS halves it tests.

## Latent MSVC /W4 breaks the full-workspace build found (all fixed)

The stage-14/15 code had only ever been compiled by GCC through the
spice-only presets; the first whole-workspace MSVC build (the `debug`
preset, /W4 /WX) failed in `tessellation.cpp` (C4456: a join-corner
`a_in` hiding the fringe alpha `a_in`) - and the same audit found more
of the same class waiting behind it. GCC's `-Wall -Wextra` does not
carry the /W4 shadowing or narrowing families, so the Linux side had
stayed silent; `tools/audit_shadow.py` had the right idea but its
default scope was the spice subset, so the ui lib had never been
scanned. Fixed, with the classes MSVC actually rejects:

- `tessellation.cpp`: the fringe alpha is `alpha_in` and the cap
  lambda's normal is `nrm` (both hid locals of the same name).
- `record.hpp`: the progress lambda reads `latest_fraction`/
  `latest_message`, and the outer guard is `rec_lock` (the lambda's
  per-iteration locks shadowed it).
- Lambda parameters no longer hide enclosing locals: the gallery's
  job lambda takes `job` (not `ctx`, the menu event), the settings
  migration test rebuilds `root` (not `doc`), and the tab-strip test
  writes through `current` (not `active`, the ActiveRenderer).
- `test_ui.cpp` also carried a latent C4189 (GCC: unused-but-set) -
  the `same_line` test's first rect now CHECKs the (0, 0) the comment
  always claimed.
- Narrowings made explicit: the box2d part id wraps the `-1u` inside
  the uint8_t cast, and the texture mip count casts `bit_width`'s
  int at the source.

`tools/audit_shadow.py` now defaults to every first-party TU (the
subset default is how this slipped through), and
`tools/audit_conversions.py` joins it for the C4244/C4245/C4267
families. Both stay GCC-side: MSVC needs no audit for what /W4
already rejects. The member-shadow findings GCC also reports
(constructor parameters named like their fields, ~800 across the
input/jobs/ui headers) are MSVC-accepted and were left alone.

### C4996: the deprecation family (the first Windows /WX build past the
shadowing fixes found it in `property_grid.cpp`)

MSVC deprecates the unchecked CRT calls (`strncpy`, `strcpy`, `sprintf`,
`fopen`, `getenv`, ...) and `/WX` makes C4996 fatal - a family GCC has
no equivalent for, so no Linux build or GCC audit can see it. The
workspace scan found exactly three sites, two already correct:

- `property_grid.cpp`'s text-buffer sync used `std::strncpy`; it is now
  `std::copy_n` + terminator - the same truncating copy with no
  deprecated call (this is the preferred shape whenever a portable
  rewrite exists, unlike the pragma case below).
- `app.cpp`'s `env_probe` keeps its scoped C4996 pragma: `getenv` is
  the portable spelling and only MSVC objects, so the suppression is
  one function, not the TU and not `_CRT_SECURE_NO_WARNINGS`.
- `playground`'s `std::fopen` asset-unpack is inside the
  `__ANDROID__` half - MSVC never compiles it, and desktop keeps
  `std::ifstream`.

## The chrome restyle (the first screenshot review)

The running example - font fixed, real file open - still looked like a
day-3 prototype: one flat gray void (the menu/toolbar strips painted
nothing and rode the r2d clear color), a saturated green accent doing
every job at once (tabs, checks, plot bars), toolbar labels clipping
into "Ne...", an invisible panel splitter, and editor text drawn like
a status line. The restyle, values-first and seam-preserving:

- ChromeTheme grows `bar_bg` - the chrome strips' own face (menu bar,
  toolbar, tab strips, status bar, dock splitters), one depth step
  away from panel_bg. Three layers now read as three layers: bar
  (23,25,30) / panel (33,35,41) / control (45,48,56), the VS Code
  depth system. theme_json carries the new face; kLightChrome moved
  to designated initializers to say it too.
- The accent is blue (96,156,224) and means "selected/live" only:
  checked boxes, the slider fill, the selected tab's underline, the
  plot's positive bars. The green is gone.
- The selected tab wears the PANEL face plus a 2-px accent underline
  at its bottom edge - it merges with the body it owns instead of
  floating on the strip; idle tabs and idle menu titles wear nothing
  (text on the bar), and the open menu title wears the popup's own
  face so title and window read as one surface.
- The toolbar buttons get fitting slack (no more clipped glyphs), the
  strips get 1-px separator rules, the splitter is a visible recessed
  gutter in bar_bg, the status bar rides bar_bg with quieter text,
  and control_height/corner_radius breathe a little more (26/5).
- shell_example: the editor is an editor now - a dim right-aligned
  number gutter, content in consola (the UI font when it refuses),
  comment lines a step quieter, and the CJK carrier stacked under
  both fonts (msyh.ttc, the gallery rule) so the utils.h Chinese
  comments render instead of tofu. The plot is a chart now: mid
  baseline, +A and zero guides, rounded bars with the negative half
  a step quieter, and axis labels off the live settings.

Both stock palettes still pass contrast_failures() and the 0.05
hover-gap contract (the dark hover was one Oklab step short at first;
the test caught it - picked (62,66,77), light (202,205,214)).
